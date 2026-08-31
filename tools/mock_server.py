#!/usr/bin/env python3
"""
mock_server.py -- a fake OpenRouter on a laptop.

    python tools/mock_server.py

Firmware points at http://<laptop-ip>:8080/api/v1 and never talks to the real
OpenRouter during development. That keeps firmware work deterministic and free,
and it is our demo-day fallback: if the venue Wi-Fi dies we point the device at
a laptop and still demo. Keep this working all four weeks.

Plain HTTP here is DELIBERATE (02-SOFTWARE.md section 3). Do not add TLS to the
mock. The point is to prove the request/response loop first, so that when TLS
breaks in week 2 we know it is TLS.

Beyond the brief's 40 lines this adds three things firmware actually needs:
  * the uploaded base64 is decoded and validated as a JPEG, which catches a
    truncated or mis-padded streaming encoder immediately instead of at the
    "why is the model describing nothing" stage;
  * every received photo is saved to tools/captures/, which is where the eval
    set's 20 real photos can come from;
  * failure modes are triggerable on demand, so the state machine's error
    paths can be tested without unplugging anything.
"""
import base64
import os
import re
import struct
import time
from datetime import datetime

from flask import Flask, Response, jsonify, request

HOST, PORT = "0.0.0.0", 8080          # 0.0.0.0 -- the ESP32 must reach this
VISION_DELAY_S = 2.5                  # imitate real latency -- DO NOT REMOVE
TTS_DELAY_S = 1.0
CAPTURE_DIR = "tools/captures"
PCM_DIR = "tools/phrases_pcm16"

app = Flask(__name__)

# ---- canned replies, one per mode ---------------------------------------
CANNED = {
    "read": ("Amoxicillin 500 milligrams. Take one capsule three times a day "
             "with food. Complete the full course."),
    "describe": ("A white medicine bottle with a blue cap, held upright about "
                 "20 centimetres away."),
    "summarise": ("This is a prescription label. Amoxicillin 500 milligrams, "
                  "three times a day with food."),
    "notext": "NOTEXT",
}

# Failure modes firmware must handle. Flip at runtime:
#   curl -X POST http://localhost:8080/mock/scenario -d scenario=notext
SCENARIOS = ("ok", "notext", "http500", "timeout", "garbage", "empty")
state = {"scenario": "ok", "presses": 0}


def log(msg):
    print(f"[mock {datetime.now():%H:%M:%S}] {msg}", flush=True)


def classify(prompt):
    """Infer the mode from the prompt so one mock serves all three."""
    p = (prompt or "").lower()
    if "transcribe" in p:
        return "read"
    if "describe what is in front" in p or "low vision" in p:
        return "describe"
    if "under 60 words" in p or "kind of document" in p:
        return "summarise"
    return "read"


def find_parts(payload):
    """Pull (prompt_text, image_data_url) out of an OpenAI-shaped body without
    assuming a fixed part order -- the brief puts text first, but a firmware
    bug that swaps them should produce a clear log line, not a KeyError."""
    text, image = None, None
    for msg in payload.get("messages", []):
        content = msg.get("content")
        if isinstance(content, str):
            text = text or content
            continue
        for part in content or []:
            if part.get("type") == "text" and text is None:
                text = part.get("text")
            elif part.get("type") == "image_url" and image is None:
                image = (part.get("image_url") or {}).get("url")
    return text, image


def jpeg_dims(raw):
    """Width/height from the first SOFn marker, or None."""
    i = 2
    while i + 9 < len(raw):
        if raw[i] != 0xFF:
            i += 1
            continue
        marker = raw[i + 1]
        if marker in (0xD8, 0xD9):
            i += 2
            continue
        seglen = struct.unpack(">H", raw[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", raw[i + 5:i + 9])
            return (w, h)
        i += 2 + seglen
    return None


def check_jpeg(data_url):
    """Decode the base64 and sanity-check the JPEG. This is the single most
    useful thing the mock does: a streaming base64 encoder that drops the tail
    or mis-pads is the likeliest firmware bug, and it is invisible from the
    device end."""
    if not data_url:
        return None, "no image part in request"
    m = re.match(r"^data:image/jpeg;base64,(.*)$", data_url, re.S)
    if not m:
        return None, ("image_url is not a data:image/jpeg;base64 URL "
                      f"(starts with {data_url[:32]!r})")
    b64 = m.group(1)
    if len(b64) % 4:
        return None, (f"base64 length {len(b64)} is not a multiple of 4 -- "
                      "truncated upload or wrong Content-Length")
    try:
        raw = base64.b64decode(b64, validate=True)
    except Exception as exc:
        return None, f"base64 will not decode: {exc}"
    if raw[:2] != b"\xff\xd8":
        return None, "decoded bytes do not start with SOI (ffd8) -- not a JPEG"
    if raw[-2:] != b"\xff\xd9":
        return None, (f"decoded {len(raw)}B but no EOI (ffd9) at the end -- "
                      "the upload is truncated; check Content-Length "
                      "(prefixLen + encodedLength() + suffixLen)")
    return {"bytes": raw, "dims": jpeg_dims(raw), "b64len": len(b64)}, None


def save_capture(raw):
    os.makedirs(CAPTURE_DIR, exist_ok=True)
    path = os.path.join(CAPTURE_DIR, f"{datetime.now():%Y%m%d-%H%M%S}.jpg")
    with open(path, "wb") as f:
        f.write(raw)
    return path


def load_pcm():
    """Return 16-bit LE mono PCM bytes for the TTS reply. Prefers a converted
    phrase; falls back to a 440 Hz tone so the mock still runs on a fresh
    clone. NOTE: the brief's snippet used Python's `wave` module, which cannot
    open the float32 WAVs in Messages/ -- run tools/wav_to_phrases.py first."""
    for name in ("no_text.wav", "ready.wav"):
        path = os.path.join(PCM_DIR, name)
        if os.path.exists(path):
            data = open(path, "rb").read()
            idx = data.find(b"data")
            if idx > 0:
                return data[idx + 8:], f"{PCM_DIR}/{name}"
    import array
    import math
    a = array.array("h", (int(12000 * math.sin(2 * math.pi * 440 * i / 24000))
                          for i in range(24000)))       # 1 s of 440 Hz
    return a.tobytes(), "generated 440 Hz tone (no converted WAVs found)"


# ---- vision -------------------------------------------------------------
@app.post("/api/v1/chat/completions")
def vision():
    state["presses"] += 1
    scenario = state["scenario"]
    log(f"--- press #{state['presses']}  scenario={scenario}")

    payload = request.get_json(force=True, silent=True)
    if payload is None:
        log("  ! body is not valid JSON")
        return jsonify({"error": {"message": "invalid JSON"}}), 400

    prompt, image = find_parts(payload)
    mode = classify(prompt)
    log(f"  model={payload.get('model')} max_tokens={payload.get('max_tokens')}")
    log(f"  mode={mode}  prompt={(prompt or '')[:60]!r}")

    info, err = check_jpeg(image)
    if err:
        log(f"  ! IMAGE REJECTED: {err}")
        return jsonify({"error": {"message": err}}), 400
    dims = f"{info['dims'][0]}x{info['dims'][1]}" if info["dims"] else "unknown"
    log(f"  image OK: {len(info['bytes']) / 1024:.1f} KB JPEG, {dims}, "
        f"{info['b64len']} chars base64")
    log(f"  saved {save_capture(info['bytes'])}")

    if scenario == "http500":
        log("  -> returning HTTP 500")
        return jsonify({"error": {"message": "mock failure"}}), 500
    if scenario == "timeout":
        log("  -> sleeping 30 s to trigger the client timeout")
        time.sleep(30)
    if scenario == "garbage":
        log("  -> returning malformed JSON")
        return Response('{"choices": [{"mess', mimetype="application/json")

    time.sleep(VISION_DELAY_S)
    text = CANNED["notext"] if scenario == "notext" else CANNED[mode]
    if scenario == "empty":
        text = ""
    log(f"  -> {text[:70]!r}")
    return jsonify({
        "id": f"gen-mock-{state['presses']}",
        "model": payload.get("model", "mock"),
        "choices": [{"index": 0, "finish_reason": "stop",
                     "message": {"role": "assistant", "content": text}}],
        "usage": {"prompt_tokens": 1200,
                  "completion_tokens": len(text) // 4,
                  "total_tokens": 1200 + len(text) // 4},
    })


# ---- text to speech -----------------------------------------------------
@app.post("/api/v1/audio/speech")
def speech():
    scenario = state["scenario"]
    payload = request.get_json(force=True, silent=True) or {}
    text = payload.get("input", "")
    log(f"  TTS in: {text[:60]!r} voice={payload.get('voice')} "
        f"format={payload.get('response_format')}")

    if payload.get("response_format") != "pcm":
        log(f"  ! response_format is {payload.get('response_format')!r}, "
            "not 'pcm' -- the device has no MP3 decoder")
    if text.strip().upper() == "NOTEXT":
        log("  ! NOTEXT was sent to TTS. Handle it on-device with the "
            "pre-recorded phrase instead -- this costs money and sounds like "
            "a malfunction. (02-SOFTWARE.md section 5)")
    if not text.strip():
        log("  ! empty input sent to TTS")

    if scenario == "http500":
        return jsonify({"error": {"message": "mock failure"}}), 500

    time.sleep(TTS_DELAY_S)
    pcm, src = load_pcm()
    secs = len(pcm) / 2 / 24000
    log(f"  -> {len(pcm)} bytes PCM ({secs:.2f}s @ 24k/16/mono) from {src}")
    return Response(pcm, mimetype="audio/pcm",
                    headers={"Content-Length": str(len(pcm))})


# ---- control surface ----------------------------------------------------
@app.post("/mock/scenario")
def set_scenario():
    want = (request.form.get("scenario")
            or (request.get_json(silent=True) or {}).get("scenario", ""))
    if want not in SCENARIOS:
        return jsonify({"error": f"scenario must be one of {SCENARIOS}"}), 400
    state["scenario"] = want
    log(f"scenario -> {want}")
    return jsonify({"scenario": want})


@app.get("/mock/status")
def status():
    return jsonify({**state, "scenarios": list(SCENARIOS)})


if __name__ == "__main__":
    _, source = load_pcm()
    print("mock_server.py -- fake OpenRouter. Plain HTTP is deliberate.")
    print(f"audio source : {source}")
    print(f"scenarios    : {', '.join(SCENARIOS)}  (POST /mock/scenario)")
    print(f"captures     : {CAPTURE_DIR}/")
    print(f"listening on : http://{HOST}:{PORT}/api/v1")
    print("               ^ point firmware at the LAN IP, not 127.0.0.1\n")
    app.run(host=HOST, port=PORT, threaded=True)
