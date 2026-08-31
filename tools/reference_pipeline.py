#!/usr/bin/env python3
"""
reference_pipeline.py -- a JPEG on disk becomes a playable WAV.

This is the ground truth the firmware imitates. If the device disagrees with
this script, the device is wrong.

    # against the mock, no key needed
    python tools/reference_pipeline.py photo.jpg --base-url http://127.0.0.1:8080/api/v1

    # against real OpenRouter
    set OPENROUTER_API_KEY=sk-or-v1-...
    python tools/reference_pipeline.py photo.jpg --mode read --out out.wav

Its other job is the day-1 task in 02-SOFTWARE.md section 4: CONFIRM THE TTS
SAMPLE RATE against a real response and tell the firmware track the number.
Pass --probe-rate and it reports what actually came back rather than trusting
the 24 kHz assumption. Get this wrong and everything plays chipmunked.
"""
import argparse
import base64
import json
import os
import struct
import sys
import time

import requests

DEFAULT_BASE = "https://openrouter.ai/api/v1"
VISION_MODEL = "google/gemini-3-flash-preview"
TTS_MODEL = "openai/gpt-4o-mini-tts-2025-12-15"
TTS_VOICE = "alloy"

PROMPTS = {
    "read": (
        "Transcribe all text visible in this image, exactly as written. "
        "Preserve the reading order a sighted person would use. Output ONLY "
        "the transcribed text -- no preamble, no description, no commentary, "
        "no markdown. If the image contains no legible text, output exactly: "
        "NOTEXT"),
    "describe": (
        "Describe what is in front of the user in one or two short sentences, "
        "as if speaking to a person with low vision who is holding this "
        "camera. Lead with the most important object. Mention text only if it "
        "identifies the object. No preamble, no markdown, under 40 words."),
    "summarise": (
        "This is a photo of a document. In under 60 words, tell the reader "
        "what kind of document it is and the single most important piece of "
        "information in it -- an amount, a date, a deadline, a dose, or an "
        "action required. Plain speech, no preamble, no markdown."),
}

# Belt-and-braces, mirrored on the device: models sometimes lead with these
# despite the prompt, and they get read aloud as noise.
PREFIXES = ("sure,", "sure!", "here is", "here's", "certainly,", "of course,",
            "the text reads", "the image shows", "this image shows")


def clean(text):
    """Strip the preamble and markdown the prompt asked the model not to emit.
    Returns (cleaned, [notes]) so prompt regressions are visible rather than
    silently patched over."""
    notes = []
    out = text.strip()
    for p in PREFIXES:
        if out.lower().startswith(p):
            out = out[len(p):].lstrip(" :,-")
            notes.append(f"stripped leading {p!r}")
            break
    if any(c in out for c in "*#`_"):
        notes.append("markdown characters present -- tighten the prompt")
        out = out.replace("**", "").replace("*", "").replace("`", "")
        out = "\n".join(line.lstrip("# ").rstrip() for line in out.splitlines())
    return out.strip(), notes


def vision(session, base, key, jpeg_path, mode, max_tokens=400):
    raw = open(jpeg_path, "rb").read()
    if raw[:2] != b"\xff\xd8":
        sys.exit(f"{jpeg_path} is not a JPEG")
    b64 = base64.b64encode(raw).decode()
    body = {
        "model": VISION_MODEL,
        "max_tokens": max_tokens,
        "messages": [{
            "role": "user",
            "content": [
                # Text part BEFORE the image -- the docs are explicit that
                # this parses better.
                {"type": "text", "text": PROMPTS[mode]},
                {"type": "image_url",
                 "image_url": {"url": f"data:image/jpeg;base64,{b64}"}},
            ],
        }],
    }
    print(f"  jpeg        : {len(raw) / 1024:.1f} KB -> {len(b64)} chars base64")
    t0 = time.time()
    r = session.post(f"{base}/chat/completions", json=body,
                     headers=auth(key), timeout=60)
    dt = time.time() - t0
    if r.status_code != 200:
        sys.exit(f"vision failed {r.status_code}: {r.text[:300]}")
    data = r.json()
    text = data["choices"][0]["message"]["content"]
    usage = data.get("usage") or {}
    print(f"  vision      : {dt:.2f}s, {usage.get('total_tokens', '?')} tokens")
    return text, dt


def tts(session, base, key, text, fmt="pcm"):
    body = {"model": TTS_MODEL, "input": text, "voice": TTS_VOICE,
            "response_format": fmt, "speed": 1.0}
    t0 = time.time()
    r = session.post(f"{base}/audio/speech", json=body,
                     headers=auth(key), timeout=120)
    dt = time.time() - t0
    if r.status_code != 200:
        sys.exit(f"tts failed {r.status_code}: {r.text[:300]}")
    print(f"  tts         : {dt:.2f}s, {len(r.content)} bytes, "
          f"content-type={r.headers.get('Content-Type')}")
    return r.content, dt, r.headers.get("Content-Type", "")


def auth(key):
    h = {"Content-Type": "application/json"}
    if key:
        h["Authorization"] = f"Bearer {key}"
    return h


def wrap_wav(pcm, rate):
    """Wrap raw 16-bit mono PCM in a WAV header so it is playable."""
    n = len(pcm) - (len(pcm) % 2)
    return (b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt "
            + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
            + b"data" + struct.pack("<I", n) + pcm[:n])


def probe_rate(pcm, claimed):
    """We cannot read a sample rate out of headerless PCM, so report what the
    byte count implies at each candidate rate and let a human listen. The only
    definitive test is playing it back -- if the voice sounds fast, the real
    rate is higher than what I2S is configured for."""
    print("\n  raw PCM has no header, so the rate cannot be read from the "
          "bytes.\n  duration if the rate were:")
    for rate in (16000, 22050, 24000, 44100, 48000):
        marker = "  <- assumed" if rate == claimed else ""
        print(f"    {rate:>6} Hz -> {len(pcm) / 2 / rate:6.2f}s{marker}")
    print("\n  Play the WAV. Correct rate = speech sounds natural. Then put "
          "the number\n  in config.h TTS_SAMPLE_RATE and tell the firmware "
          "track. (section 4)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--mode", choices=list(PROMPTS), default="read")
    ap.add_argument("--out", default="out.wav")
    ap.add_argument("--base-url", default=os.environ.get(
        "OPENROUTER_BASE_URL", DEFAULT_BASE))
    ap.add_argument("--rate", type=int, default=24000,
                    help="rate to write into the WAV header")
    ap.add_argument("--probe-rate", action="store_true",
                    help="report candidate durations to confirm the real rate")
    ap.add_argument("--text-only", action="store_true",
                    help="skip TTS (free -- use this while tuning prompts)")
    args = ap.parse_args()

    key = os.environ.get("OPENROUTER_API_KEY", "")
    is_mock = "openrouter.ai" not in args.base_url
    if not key and not is_mock:
        sys.exit("OPENROUTER_API_KEY is not set. Either export it, or point "
                 "--base-url at the mock server.")

    print(f"\n  backend     : {args.base_url}"
          f"{'  (mock -- plain HTTP)' if is_mock else '  (real, TLS)'}")
    print(f"  mode        : {args.mode}")

    session = requests.Session()
    t_start = time.time()
    raw_text, t_vision = vision(session, args.base_url, key, args.image,
                                args.mode)
    text, notes = clean(raw_text)
    print(f"  raw         : {raw_text[:100]!r}")
    for n in notes:
        print(f"  ! {n}")
    print(f"  text        : {text!r}")

    if text.strip().upper() == "NOTEXT":
        print("\n  NOTEXT -- on the device this plays the pre-recorded "
              '"Nothing found" phrase.\n  It must never be sent to TTS: it '
              "costs money and sounds like a malfunction.")
        return 0
    if args.text_only:
        return 0

    pcm, t_tts, ctype = tts(session, args.base_url, key, text)
    if "json" in ctype:
        sys.exit("TTS returned JSON, not audio -- check response_format=pcm")

    if len(pcm) % 2:
        print("  ! odd byte count -- a 16-bit sample is split. On the device "
              "this is\n    the one-byte carry across chunk boundaries "
              "(section 6.2/11.4).")

    with open(args.out, "wb") as f:
        f.write(wrap_wav(pcm, args.rate))
    secs = len(pcm) / 2 / args.rate
    print(f"  wrote       : {args.out}  ({secs:.2f}s at {args.rate} Hz)")

    if args.probe_rate:
        probe_rate(pcm, args.rate)

    print(f"\n  TOTAL       : {time.time() - t_start:.2f}s "
          f"(vision {t_vision:.2f}s + tts {t_tts:.2f}s)")
    print(f"  chars to TTS: {len(text)}  (billed per input character)")
    print("  target      : first spoken word under 6 s\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
