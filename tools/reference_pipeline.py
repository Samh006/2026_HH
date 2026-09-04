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

The speech leg follows D14, NOT section 4 of the software brief: that section
describes /audio/speech with response_format: pcm, which does not exist on
OpenRouter. See tts() for the shape that actually works.

The sample rate question the brief raises on day 1 is closed -- 24 kHz 16-bit
mono, confirmed in D15. --probe-rate re-checks it if a model ever changes.
"""
import argparse
import base64
import json
import os
import re
import struct
import sys
import time

import requests

DEFAULT_BASE = "https://openrouter.ai/api/v1"
# D16 measured gemini-3.5-flash-lite as 1.4 s faster and 37% cheaper at equal
# accuracy, but explicitly says do not switch on synthetic images alone.
# Confirm on the 20-photo eval set, then change this line and config.h.
VISION_MODEL = "google/gemini-3-flash-preview"
# D14: the only OpenRouter models that emit speech are gpt-audio / gpt-audio-mini,
# and they do it through /chat/completions, not /audio/speech. See tts().
TTS_MODEL = "openai/gpt-audio-mini"
TTS_VOICE = "alloy"
TTS_FORMAT = "pcm16"

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


def tts(session, base, key, text, voice=TTS_VOICE, fmt=TTS_FORMAT):
    """Speech leg -- D14. OpenRouter has no /audio/speech endpoint and no
    response_format: pcm; both return 400. Speech is a chat completion asked
    for the audio modality, and audio output is REFUSED unless stream is true.
    The bytes arrive base64-encoded inside SSE deltas at
    choices[0].delta.audio.data.

    Written the way firmware has to write it, because this is the file the
    device copies: decode each delta as it arrives, keep a one-byte carry so a
    16-bit sample is never split across an i2s_write(), and never buffer the
    whole utterance waiting for [DONE].

    Returns (pcm, seconds_to_first_audio_byte, seconds_to_complete). The first
    of those is the number that matters -- it is when the user hears speech."""
    body = {
        "model": TTS_MODEL,
        "modalities": ["text", "audio"],
        "audio": {"voice": voice, "format": fmt},
        "stream": True,          # not optional: audio is refused without it
        "messages": [{"role": "user", "content": text}],
    }
    t0 = time.time()
    r = session.post(f"{base}/chat/completions", json=body,
                     headers=auth(key), stream=True, timeout=120)
    if r.status_code != 200:
        sys.exit(f"tts failed {r.status_code}: {r.text[:300]}")

    pcm = bytearray()
    carry = b""              # the odd byte -- half of a 16-bit sample
    chunks = 0
    t_first = None
    transcript = ""
    for line in r.iter_lines(decode_unicode=True):
        if not line or not line.startswith("data:"):
            continue
        payload = line[5:].strip()
        if payload == "[DONE]":
            break
        try:
            delta = json.loads(payload)["choices"][0].get("delta") or {}
        except (ValueError, KeyError, IndexError):
            continue                      # keep-alives and non-audio deltas
        audio = delta.get("audio") or {}
        transcript = audio.get("transcript") or transcript
        b64 = audio.get("data")
        if not b64:
            continue
        if t_first is None:
            t_first = time.time() - t0
        chunks += 1
        # Each delta carries its own padded base64 blob, so it decodes
        # standalone -- there is no base64 carry across deltas. What does NOT
        # survive is sample alignment: an odd-length decode splits a 16-bit
        # sample, which on the device is a periodic click (section 11.4).
        buf = carry + base64.b64decode(b64)
        carry, buf = (buf[-1:], buf[:-1]) if len(buf) % 2 else (b"", buf)
        pcm += buf                        # <- on the device this is i2s_write()

    dt = time.time() - t0
    if not pcm:
        sys.exit("tts returned no audio deltas -- check modalities and stream")
    if carry:
        print("  ! stream ended mid-sample; one trailing byte dropped")
    print(f"  tts         : first byte {t_first:.2f}s, complete {dt:.2f}s, "
          f"{chunks} chunks, {len(pcm)} bytes")
    if transcript:
        print(f"  transcript  : {transcript[:70]!r}")
    return bytes(pcm), t_first, dt


def auth(key):
    h = {"Content-Type": "application/json"}
    if key:
        h["Authorization"] = f"Bearer {key}"
    return h


CONFIG_H = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "firmware", "src", "config.h")


def load_key():
    """OPENROUTER_API_KEY if set, else read OR_API_KEY out of config.h.

    The fallback matters for two reasons: config.h is gitignored and already
    holds the key for the firmware, so there is one place to update rather than
    two; and a freshly-set Windows user environment variable is invisible to
    processes that were already running, which otherwise looks like a missing
    key. Never print the return value."""
    key = os.environ.get("OPENROUTER_API_KEY", "").strip()
    if key:
        return key, "environment"
    try:
        with open(CONFIG_H, encoding="utf-8", errors="replace") as f:
            m = re.search(r'#define\s+OR_API_KEY\s+"([^"]*)"', f.read())
    except OSError:
        return "", "not found"
    if not m or "REPLACE-ME" in m.group(1):
        return "", "config.h placeholder"
    return m.group(1).strip(), "firmware/src/config.h"


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

    key, key_src = load_key()
    is_mock = "openrouter.ai" not in args.base_url
    if not key and not is_mock:
        sys.exit(f"No API key ({key_src}). Set OPENROUTER_API_KEY, or fill in "
                 "OR_API_KEY in firmware/src/config.h, or point --base-url at "
                 "the mock server.")

    print(f"\n  backend     : {args.base_url}"
          f"{'  (mock -- plain HTTP)' if is_mock else '  (real, TLS)'}")
    if not is_mock:
        print(f"  key         : {len(key)} chars, from {key_src}")
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

    pcm, t_first, t_tts = tts(session, args.base_url, key, text)

    with open(args.out, "wb") as f:
        f.write(wrap_wav(pcm, args.rate))
    secs = len(pcm) / 2 / args.rate
    print(f"  wrote       : {args.out}  ({secs:.2f}s at {args.rate} Hz)")

    if args.probe_rate:
        probe_rate(pcm, args.rate)

    print(f"\n  TOTAL       : {time.time() - t_start:.2f}s "
          f"(vision {t_vision:.2f}s + tts to completion {t_tts:.2f}s)")
    # The cloud half of the 6 s budget. Device capture, upload and I2S start
    # are on top of this, and are not measured here.
    first_word = t_vision + t_first
    verdict = "OK" if first_word < 6 else "OVER"
    print(f"  first word  : {first_word:.2f}s cloud-only  "
          f"(vision {t_vision:.2f}s + TTS first byte {t_first:.2f}s)  "
          f"[{verdict} vs 6 s]")
    print(f"  chars to TTS: {len(text)}  (billed per input character)\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
