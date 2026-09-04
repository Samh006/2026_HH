#!/usr/bin/env python3
"""
mock_server.py -- a fake OpenRouter on a laptop.

    python tools/mock_server.py

Firmware points at http://<laptop-ip>:8080/api/v1 and never talks to the real
OpenRouter during development. That keeps firmware work deterministic and free,
and it is our demo-day fallback: if the venue Wi-Fi dies we point the device at
a laptop and still demo. Keep this working all four weeks.

Plain HTTP here is DELIBERATE (D10). Do not add TLS to the mock. The point is
to prove the request/response loop first, so that when TLS breaks in week 2 we
know it is TLS.

IMPORTANT -- this mock now matches the REAL OpenRouter shape, which is not what
the software brief described (see D14). Verified against the live API:

  * There is no usable /audio/speech endpoint and no response_format: pcm.
    This mock returns the real 400 for it, so firmware finds out immediately
    rather than building against a fiction.
  * Speech comes from openai/gpt-audio-mini on /chat/completions with
    stream: true, modalities: ["text","audio"], audio: {voice, format}.
    Audio arrives as base64 inside SSE `delta.audio.data` chunks.
  * Audio output without stream: true is refused. This mock refuses it too.

So firmware's audio path needs SSE line framing plus a streaming base64
DECODER, and two carries: base64 quads (4 chars -> 3 bytes) and 16-bit sample
alignment. The mock chunks on deliberately awkward boundaries to shake both out.
"""
import base64
import json
import os
import re
import struct
import time
from datetime import datetime

from flask import Flask, Response, jsonify, request

HOST, PORT = "0.0.0.0", 8080          # 0.0.0.0 -- the ESP32 must reach this
VISION_DELAY_S = 2.5                  # imitate real latency -- DO NOT REMOVE
TTS_FIRST_BYTE_S = 1.6                # measured against the real API
TTS_CHUNKS = 13                       # measured: 13 chunks for ~5 s of speech
CAPTURE_DIR = "tools/captures"
PCM_DIR = "tools/phrases_pcm16"
SAMPLE_RATE = 24000

app = Flask(__name__)

CANNED = {
    "read": ("Amoxicillin 500 milligrams. Take one capsule three times a day "
             "with food. Complete the full course."),
    "describe": ("A white medicine bottle with a blue cap, held upright about "
                 "20 centimetres away."),
    "summarise": ("This is a prescription label. Amoxicillin 500 milligrams, "
                  "three times a day with food."),
    "notext": "NOTEXT",
    # D17: what a degraded image should look like coming back
    "uncertain": ("Metformin 850 milligrams. Take 1 tablet twice daily with "
                  "meals. Qty 60 UNCLEAR Exp 09/2026 UNCLEAR"),
}

SCENARIOS = ("ok", "notext", "uncertain", "http500", "timeout", "garbage",
             "empty")
state = {"scenario": "ok", "presses": 0}


def log(msg):
    print(f"[mock {datetime.now():%H:%M:%S}] {msg}", flush=True)


def classify(prompt):
    p = (prompt or "").lower()
    if "transcribe" in p:
        return "read"
    if "describe what is in front" in p or "low vision" in p:
        return "describe"
    if "under 60 words" in p or "kind of document" in p:
        return "summarise"
    return "read"


def find_parts(payload):
    """(prompt_text, image_data_url) without assuming part order."""
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
    """Decode and validate. The single most useful thing the mock does: a
    streaming base64 encoder that drops its tail or mis-pads is the likeliest
    firmware bug and is invisible from the device end."""
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
    """16-bit LE mono PCM for the audio reply, or a 440 Hz tone as fallback."""
    # mock_tts_sample.wav FIRST, and this ordering matters. The fallbacks are
    # system phrases, so with no_internet.wav the device announces "No internet
    # connection" while perfectly connected -- and since this mock is the
    # DEMO-DAY FALLBACK (02-SOFTWARE.md gotcha 7), that means demoing a
    # medicine label to judges with the device claiming it is offline.
    # mock_tts_sample.wav speaks the same text the vision endpoint returns, so
    # the mock behaves like the real API end to end.
    # Regenerate with pocket-tts if the canned text changes.
    for name, base in (("mock_tts_sample.wav", "tools"),
                       ("no_internet.wav", PCM_DIR),
                       ("no_text.wav", PCM_DIR),
                       ("ready.wav", PCM_DIR)):
        path = os.path.join(base, name)
        if os.path.exists(path):
            data = open(path, "rb").read()
            idx = data.find(b"data")
            if idx > 0:
                return data[idx + 8:], path
    import array
    import math
    a = array.array("h", (int(12000 * math.sin(2 * math.pi * 440 * i / 24000))
                          for i in range(24000)))
    return a.tobytes(), "generated 440 Hz tone (run wav_to_phrases.py)"


def sse(obj):
    # COMPACT separators, matching the real API on the wire. The default
    # json.dumps emits '"data": "..."' with a space after the colon; a
    # firmware parser doing a literal strstr for '"data":"' then skips
    # every audio delta -- 13 chunks sent, 0 received, and no error anywhere
    # to point at it. A mock that differs from the real API by one byte is
    # worse than no mock.
    return f"data: {json.dumps(obj, separators=(',', ':'))}\n\n"


def audio_stream(text, voice, fmt):
    """SSE stream shaped like the real thing: a role delta, then audio deltas
    carrying base64 PCM, then [DONE].

    The chunk sizes are deliberately NOT multiples of 3 bytes, so each base64
    payload ends mid-quad and each decoded chunk can end on an odd byte. That
    is exactly what the real API does and it is what breaks a naive decoder:
    firmware needs a base64 carry AND a 16-bit sample carry."""
    pcm, src = load_pcm()
    log(f"  audio source {src}, {len(pcm)} bytes")
    yield sse({"choices": [{"index": 0, "delta": {"role": "assistant"}}]})
    time.sleep(TTS_FIRST_BYTE_S)

    # awkward sizes on purpose -- 3-byte-aligned chunks would hide the bug
    step = max(1, len(pcm) // TTS_CHUNKS) + 1
    sent = 0
    for i in range(0, len(pcm), step):
        chunk = pcm[i:i + step]
        sent += 1
        payload = {"choices": [{"index": 0, "delta": {"audio": {
            "data": base64.b64encode(chunk).decode()}}}]}
        if sent == 1:
            payload["choices"][0]["delta"]["audio"]["transcript"] = text
        yield sse(payload)
        time.sleep(0.05)
    log(f"  streamed {sent} audio chunks, {len(pcm)} bytes total "
        f"({len(pcm) / 2 / SAMPLE_RATE:.2f}s @ {SAMPLE_RATE} Hz)")
    yield sse({"choices": [{"index": 0, "delta": {},
                            "finish_reason": "stop"}]})
    yield "data: [DONE]\n\n"


@app.post("/api/v1/chat/completions")
def chat_completions():
    payload = request.get_json(force=True, silent=True)
    if payload is None:
        log("  ! body is not valid JSON")
        return jsonify({"error": {"message": "invalid JSON"}}), 400

    modalities = payload.get("modalities") or []
    wants_audio = "audio" in modalities

    # ---- speech leg ----
    if wants_audio:
        text = ""
        for msg in payload.get("messages", []):
            c = msg.get("content")
            if isinstance(c, str):
                text = c
        audio_cfg = payload.get("audio") or {}
        log(f"  TTS request: model={payload.get('model')} "
            f"voice={audio_cfg.get('voice')} format={audio_cfg.get('format')}")
        log(f"  input: {text[:70]!r}")

        # The real API refuses audio output without streaming. Mirror it.
        if not payload.get("stream"):
            log("  ! rejected: audio output requires stream: true")
            return jsonify({"error": {
                "message": "Audio output requires stream: true",
                "code": 400}}), 400
        if audio_cfg.get("format") not in ("pcm16", "wav"):
            log(f"  ! format {audio_cfg.get('format')!r} -- want pcm16 "
                "(the device has no decoder)")
        if "UNCLEAR" in text or "[?]" in text:
            log("  ! text still contains uncertainty markers. Strip them on "
                "device and speak PHRASE_UNCERTAIN instead (D17).")
        if text.strip().upper() == "NOTEXT":
            log("  ! NOTEXT sent to TTS -- handle on device with "
                "PHRASE_NO_TEXT; this costs money and sounds broken.")
        if state["scenario"] == "http500":
            return jsonify({"error": {"message": "mock failure"}}), 500
        return Response(audio_stream(text, audio_cfg.get("voice"),
                                     audio_cfg.get("format")),
                        mimetype="text/event-stream",
                        headers={"Cache-Control": "no-cache",
                                 "X-Accel-Buffering": "no"})

    # ---- vision leg ----
    state["presses"] += 1
    scenario = state["scenario"]
    log(f"--- press #{state['presses']}  scenario={scenario}")
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
    text = CANNED["notext"] if scenario == "notext" else CANNED[
        "uncertain" if scenario == "uncertain" else mode]
    if scenario == "empty":
        text = ""
    log(f"  -> {text[:70]!r}")
    return jsonify({
        "id": f"gen-mock-{state['presses']}",
        "model": payload.get("model", "mock"),
        "choices": [{"index": 0, "finish_reason": "stop",
                     "message": {"role": "assistant", "content": text}}],
        "usage": {"prompt_tokens": 1200, "completion_tokens": len(text) // 4,
                  "total_tokens": 1200 + len(text) // 4},
    })


@app.post("/api/v1/audio/speech")
def speech_gone():
    """The real OpenRouter returns exactly this. Kept so that firmware written
    against the old brief fails loudly here instead of on demo day."""
    log("  ! /audio/speech called -- this endpoint does not work on "
        "OpenRouter (D14). Use /chat/completions with modalities+audio+stream.")
    return jsonify({"error": {
        "message": f"Model {(request.get_json(silent=True) or {}).get('model')} "
                   "does not exist", "code": 400}}), 400


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
    print("mock_server.py -- fake OpenRouter, matching the REAL API shape (D14)")
    print(f"audio source : {source}")
    print(f"scenarios    : {', '.join(SCENARIOS)}  (POST /mock/scenario)")
    print(f"captures     : {CAPTURE_DIR}/")
    print("speech       : /chat/completions + modalities:[text,audio] + "
          "stream:true  -> SSE base64")
    print("             : /audio/speech returns 400, exactly like the real API")
    print(f"listening on : http://{HOST}:{PORT}/api/v1")
    print("               ^ point firmware at the LAN IP, not 127.0.0.1\n")
    app.run(host=HOST, port=PORT, threaded=True)
