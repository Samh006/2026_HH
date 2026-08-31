#!/usr/bin/env python3
"""
make_phrases.py -- generate the offline phrase bank with pocket-tts.

    pip install pocket-tts
    python tools/make_phrases.py --voice-sample tools/cloud_voice_sample.wav

ORDER MATTERS. Do not run this until the cloud TTS voice is chosen, because
the whole point is voice cloning: sample the OpenRouter voice we picked, clone
it, and generate the phrases in that voice, so offline phrases and cloud speech
are ONE voice rather than two (00-TEAM-PLAN.md D6). Generating first and
choosing the voice later means doing this twice.

To get the sample:
    python tools/reference_pipeline.py <any.jpg> --out tools/cloud_voice_sample.wav

This writes WAVs into Messages/, then hands off to wav_to_phrases.py to do the
int16 conversion and emit firmware/src/phrases.h -- one conversion path, so the
recorded-by-hand and generated phrases cannot drift in format.
"""
import argparse
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wav_to_phrases import trim_silence   # one trim, shared with the
                                          # hand-recorded path (D7)

OUT_DIR = "Messages"
TARGET_SR = 24000

# The device must never be silent, so every state that can be reached needs a
# phrase. Keep each under 2 seconds (48 KB at 24 kHz 16-bit).
PHRASES = {
    "ready":       "Ready",
    "reading":     "Reading",
    "describing":  "Describing",
    "no_text":     "Nothing found. Try moving closer.",
    "no_internet": "No internet connection.",
    "connecting":  "Connecting.",
    "batt_low":    "Battery low.",
    "error":       "Something went wrong.",
    # D17: spoken when the model flags UNCLEAR near a number.
    "uncertain":   "I am not certain of this. Try more light, or move closer.",
    "repeating":   "Repeating.",
}


def to_numpy(audio):
    """generate_audio returns a torch.Tensor, and the mimi config declares
    channels: 1, so it arrives with a leading channel/batch dim. Flatten it --
    feeding a 2-D array to the WAV writer silently interleaves nonsense."""
    import torch
    if isinstance(audio, torch.Tensor):
        audio = audio.detach().cpu().float().numpy()
    return np.asarray(audio, dtype=np.float32).reshape(-1)


def write_wav_f32_as_int16(path, x, sr):
    """pocket-tts hands back float samples. Write int16 directly -- writing
    float32 is exactly the bug we already had to fix in the hand-recorded
    phrases: i2s_write() is configured for 16-bit, so float bytes play as
    full-scale noise and cost double the flash."""
    import struct
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    if peak > 0:
        x = x * (10.0 ** (-1.0 / 20.0) / peak)          # -1 dBFS
    pcm = np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2")
    n = pcm.nbytes
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt "
                + struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 2, 2, 16)
                + b"data" + struct.pack("<I", n))
        f.write(pcm.tobytes())
    return pcm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice-sample",
                    help="WAV of the chosen OpenRouter TTS voice, to clone. "
                         "NOTE: cloning needs the gated kyutai/pocket-tts "
                         "weights -- accept the terms on Hugging Face and log "
                         "in, or use --voice instead.")
    ap.add_argument("--voice",
                    help="a built-in catalog voice name (e.g. alba, vera, "
                         "george). Works with the ungated weights.")
    ap.add_argument("--only", nargs="*",
                    help="generate only these keys (default: all missing)")
    ap.add_argument("--force", action="store_true",
                    help="regenerate phrases that already exist in Messages/")
    ap.add_argument("--out-dir", default=OUT_DIR,
                    help="where to write WAVs; point elsewhere to try a voice "
                         "without touching the real phrase bank")
    ap.add_argument("--max-tokens", type=int, default=50,
                    help="mimi runs at 12.5 frames/s, so 50 tokens is a hard "
                         "4.0 s ceiling. Our phrases are under 2 s; raise this "
                         "only if one gets cut off mid-word.")
    args = ap.parse_args()

    try:
        from pocket_tts import TTSModel
    except ImportError:
        sys.exit("pocket-tts is not installed:  pip install pocket-tts")

    if bool(args.voice_sample) == bool(args.voice):
        sys.exit("pass exactly one of --voice-sample <wav> (clone, gated) or "
                 "--voice <name> (catalog, ungated).")
    if args.voice_sample and not os.path.exists(args.voice_sample):
        sys.exit(f"voice sample not found: {args.voice_sample}\n"
                 "Capture one from the chosen cloud voice first -- see the "
                 "docstring. Generating in a different voice than the cloud "
                 "TTS is what makes the device sound assembled rather than "
                 "finished.")

    os.makedirs(args.out_dir, exist_ok=True)
    todo = args.only or list(PHRASES)
    print("  loading model (first run downloads weights from Hugging Face)...")
    model = TTSModel.load_model()

    # Read the rate off the model rather than trusting the constant: a
    # different language config could ship a different mimi sample_rate, and
    # writing the wrong number into the WAV header is the chipmunk bug.
    sr = int(model.config.mimi.sample_rate)
    if sr != TARGET_SR:
        sys.exit(f"model outputs {sr} Hz but the firmware expects "
                 f"{TARGET_SR} Hz. Resample before writing, or change "
                 f"TTS_SAMPLE_RATE in config.h -- do not just relabel the "
                 f"header.")
    print(f"  model sample rate: {sr} Hz (matches firmware)")

    want = args.voice or args.voice_sample
    try:
        voice = model.get_state_for_audio_prompt(want)
    except ValueError as exc:
        if "voice cloning" not in str(exc).lower():
            raise
        sys.exit(
            "\n  VOICE CLONING IS NOT AVAILABLE with the weights that were\n"
            "  downloaded. kyutai/pocket-tts is a GATED model, so it silently\n"
            "  fell back to pocket-tts-without-voice-cloning.\n\n"
            "  Two ways forward, and this is a team decision (see D6/D13):\n\n"
            "  A. Get cloning. Accept the terms at\n"
            "       https://huggingface.co/kyutai/pocket-tts\n"
            "     then log in locally (`hf auth login`) and re-run. Keeps the\n"
            "     one-voice-throughout goal intact.\n\n"
            "  B. Drop cloning. Use a catalog voice:\n"
            "       python tools/make_phrases.py --voice alba\n"
            "     Then pick the OpenRouter TTS voice that sounds CLOSEST to it.\n"
            "     Two similar voices instead of one identical voice -- less\n"
            "     polished, but no gating risk before demo day.\n\n"
            f"  Original error: {exc}")
    print(f"  voice: {want}")

    made, skipped = [], []
    for key in todo:
        if key not in PHRASES:
            sys.exit(f"unknown phrase key {key!r}; known: {', '.join(PHRASES)}")
        path = os.path.join(args.out_dir, f"{key}.wav")
        if os.path.exists(path) and not args.force:
            skipped.append(key)
            continue
        audio = to_numpy(model.generate_audio(voice, PHRASES[key],
                                              max_tokens=args.max_tokens))
        raw_secs = len(audio) / sr
        # Trim with the SAME function wav_to_phrases.py uses, so the duration
        # reported here is the duration that actually reaches flash. Measuring
        # untrimmed audio false-alarms on leading silence.
        audio = trim_silence(audio, sr)
        pcm = write_wav_f32_as_int16(path, audio, sr)
        secs = len(pcm) / sr
        flags = []
        if secs > 2.0:
            flags.append("over 2s, shorten it")
        # 12.5 frames/s: landing on the ceiling means it was cut off, not that
        # the phrase happened to be exactly that long.
        if abs(raw_secs - args.max_tokens / 12.5) < 0.05:
            flags.append(f"hit the {args.max_tokens}-token ceiling -- "
                         "likely truncated, raise --max-tokens")
        flag = ("  ! " + "; ".join(flags)) if flags else ""
        print(f"  {key:12s} {secs:5.2f}s  {pcm.nbytes / 1024:6.1f} KB  "
              f"{PHRASES[key]!r}{flag}")
        made.append(key)

    if skipped:
        print(f"\n  already present (use --force to redo): {', '.join(skipped)}")
    if not made:
        print("  nothing to generate")
        return 0

    if os.path.abspath(args.out_dir) != os.path.abspath(OUT_DIR):
        print(f"\n  wrote to {args.out_dir} -- phrases.h NOT regenerated, "
              f"since wav_to_phrases.py reads {OUT_DIR}/.\n"
              f"  Listen to these first; copy them into {OUT_DIR}/ when happy.")
        return 0

    print("\n  regenerating firmware/src/phrases.h ...")
    return subprocess.call([sys.executable, "tools/wav_to_phrases.py",
                            "--emit-header"])


if __name__ == "__main__":
    sys.exit(main())
