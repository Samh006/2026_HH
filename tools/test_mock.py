#!/usr/bin/env python3
"""
test_mock.py -- exercise the mock server the way the firmware will.

    python tools/mock_server.py          # in one terminal
    python tools/test_mock.py            # in another

Run this before telling the firmware track the mock is ready, and again after
any change to it. It also documents the exact request shape firmware must
produce, including the truncated-upload case the mock is there to catch.
"""
import base64
import io
import json
import sys

import requests
from PIL import Image, ImageDraw

BASE = "http://127.0.0.1:8080"
PROMPT_READ = (
    "Transcribe all text visible in this image, exactly as written. Preserve "
    "the reading order a sighted person would use. Output ONLY the transcribed "
    "text -- no preamble, no description, no commentary, no markdown. If the "
    "image contains no legible text, output exactly: NOTEXT")

passed, failed = 0, 0


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}  {detail}")


def fake_label_jpeg(size=(800, 600), quality=12):
    """An SVGA JPEG at the quality the camera is configured for, so the logged
    byte size is representative of a real capture."""
    img = Image.new("RGB", size, "white")
    d = ImageDraw.Draw(img)
    d.rectangle([60, 120, 740, 480], outline="black", width=3)
    d.text((90, 170), "AMOXICILLIN 500mg", fill="black")
    d.text((90, 220), "Take ONE capsule THREE times a day", fill="black")
    d.text((90, 260), "with food. Complete the full course.", fill="black")
    d.text((90, 340), "Exp 09/2027   Qty 21", fill="black")
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=quality)
    return buf.getvalue()


def vision_body(jpeg_bytes, prompt=PROMPT_READ, b64=None):
    b64 = b64 if b64 is not None else base64.b64encode(jpeg_bytes).decode()
    return {
        "model": "google/gemini-3-flash-preview",
        "max_tokens": 400,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": prompt},
                {"type": "image_url",
                 "image_url": {"url": f"data:image/jpeg;base64,{b64}"}},
            ],
        }],
    }


def set_scenario(name):
    r = requests.post(f"{BASE}/mock/scenario", data={"scenario": name},
                      timeout=5)
    r.raise_for_status()


def main():
    try:
        requests.get(f"{BASE}/mock/status", timeout=3)
    except requests.RequestException:
        sys.exit("mock server not running -- start tools/mock_server.py first")

    jpeg = fake_label_jpeg()
    print(f"\ntest JPEG: {len(jpeg) / 1024:.1f} KB at SVGA q12, "
          f"{len(base64.b64encode(jpeg))} chars of base64\n")

    print("happy path")
    set_scenario("ok")
    r = requests.post(f"{BASE}/api/v1/chat/completions",
                      json=vision_body(jpeg), timeout=40)
    check("vision returns 200", r.status_code == 200, r.text[:120])
    text = r.json()["choices"][0]["message"]["content"]
    check("vision returns canned text", "Amoxicillin" in text, repr(text))

    print("\nspeech leg -- SSE audio (D14 shape)")
    r = requests.post(f"{BASE}/api/v1/audio/speech", timeout=30,
                      json={"model": "openai/gpt-audio-mini", "input": text})
    check("/audio/speech returns 400 like the real API", r.status_code == 400,
          r.text[:120])

    body = {"model": "openai/gpt-audio-mini",
            "modalities": ["text", "audio"],
            "audio": {"voice": "alloy", "format": "pcm16"},
            "messages": [{"role": "user", "content": text}]}
    r = requests.post(f"{BASE}/api/v1/chat/completions", json=body, timeout=30)
    check("audio without stream:true is refused", r.status_code == 400,
          r.text[:120])

    body["stream"] = True
    r = requests.post(f"{BASE}/api/v1/chat/completions", json=body,
                      timeout=60, stream=True)
    check("streaming audio returns 200", r.status_code == 200)
    check("content-type is text/event-stream",
          "event-stream" in (r.headers.get("Content-Type") or ""))

    pcm, chunks, odd_ends, transcript, saw_done = bytearray(), 0, 0, "", False
    for line in r.iter_lines(decode_unicode=True):
        if not line or not line.startswith("data: "):
            continue
        raw = line[6:]
        if raw.strip() == "[DONE]":
            saw_done = True
            break
        ev = json.loads(raw)
        for chd in ev.get("choices", []):
            aud = (chd.get("delta") or {}).get("audio") or {}
            if aud.get("transcript"):
                transcript = aud["transcript"]
            if aud.get("data"):
                piece = base64.b64decode(aud["data"])
                if len(piece) % 2:
                    odd_ends += 1
                pcm += piece
                chunks += 1
    check("received multiple audio chunks", chunks > 1, f"{chunks} chunks")
    check("stream terminated with [DONE]", saw_done)
    check("transcript delivered", bool(transcript), repr(transcript[:40]))
    check("chunks end mid-sample (firmware needs a carry)",
          odd_ends > 0, f"{odd_ends} of {chunks} ended on an odd byte")
    check("reassembled PCM is a whole number of samples",
          len(pcm) % 2 == 0, f"{len(pcm)} bytes")
    check("reassembled PCM is a plausible length", len(pcm) > 20000,
          f"{len(pcm)} bytes")
    print(f"        -> {chunks} chunks, {len(pcm)} bytes = "
          f"{len(pcm) / 2 / 24000:.2f}s at 24k/16/mono")

    print("\nmode routing")
    for prompt, want in [
            ("Describe what is in front of the user in one or two short "
             "sentences, as if speaking to a person with low vision", "bottle"),
            ("This is a photo of a document. In under 60 words, tell the "
             "reader what kind of document it is", "prescription")]:
        r = requests.post(f"{BASE}/api/v1/chat/completions", timeout=40,
                          json=vision_body(jpeg, prompt=prompt))
        got = r.json()["choices"][0]["message"]["content"]
        check(f"prompt routes to {want!r}", want in got.lower(), repr(got[:60]))

    print("\nupload validation (the bugs firmware will actually hit)")
    good = base64.b64encode(jpeg).decode()

    r = requests.post(f"{BASE}/api/v1/chat/completions", timeout=40,
                      json=vision_body(jpeg, b64=good[:-4]))
    check("truncated base64 rejected", r.status_code == 400)
    check("  ...and the error names the cause",
          "truncated" in r.text.lower() or "eoi" in r.text.lower(),
          r.text[:160])

    r = requests.post(f"{BASE}/api/v1/chat/completions", timeout=40,
                      json=vision_body(jpeg, b64=good[:-1]))
    check("mis-padded base64 rejected", r.status_code == 400)

    r = requests.post(f"{BASE}/api/v1/chat/completions", timeout=40,
                      json=vision_body(jpeg, b64=good[8:]))
    check("missing JPEG header rejected", r.status_code == 400)

    print("\nfailure scenarios")
    set_scenario("notext")
    r = requests.post(f"{BASE}/api/v1/chat/completions", timeout=40,
                      json=vision_body(jpeg))
    check("notext scenario returns NOTEXT",
          r.json()["choices"][0]["message"]["content"] == "NOTEXT")

    set_scenario("http500")
    r = requests.post(f"{BASE}/api/v1/chat/completions", timeout=40,
                      json=vision_body(jpeg))
    check("http500 scenario returns 500", r.status_code == 500)

    set_scenario("garbage")
    r = requests.post(f"{BASE}/api/v1/chat/completions", timeout=40,
                      json=vision_body(jpeg))
    ok = False
    try:
        r.json()
    except ValueError:
        ok = True
    check("garbage scenario returns unparseable JSON", ok)

    set_scenario("ok")
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
