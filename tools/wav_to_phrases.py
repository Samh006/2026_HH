#!/usr/bin/env python3
"""
wav_to_phrases.py -- turn the recorded phrase WAVs into 16-bit PCM and, from
that, firmware/src/phrases.h.

WHY THIS EXISTS: the WAVs in Messages/ are 32-bit IEEE float (WAV format
tag 3). i2s_write() is configured for I2S_BITS_PER_SAMPLE_16BIT, so float
bytes fed to it play as full-scale noise, and they cost double the flash.
Python's own `wave` module also refuses to open format-tag-3 files, which is
why the mock server cannot read them as-is.

Usage:
    python tools/wav_to_phrases.py                 # convert + report only
    python tools/wav_to_phrases.py --emit-header   # also write phrases.h
"""
import argparse, os, struct, sys
import numpy as np

SRC_DIR   = "Messages"
OUT_DIR   = "tools/phrases_pcm16"
HEADER    = "firmware/src/phrases.h"
TARGET_SR = 24000
MAX_SEC   = 2.0          # 02-SOFTWARE.md §8 -- keep every phrase under 2 s
TRIM_DB   = -45.0        # silence floor for edge trimming

# filename stem -> PHRASE_<NAME> in the generated header
NAME_MAP = {
    "ready":       "READY",
    "no_text":     "NO_TEXT",
    "no_internet": "NO_INTERNET",
    "reading":     "READING",
    "describing":  "DESCRIBING",
    "connecting":  "CONNECTING",
    "batt_low":    "BATT_LOW",
    "error":       "ERROR",
    "uncertain":   "UNCERTAIN",
    "repeating":   "REPEATING",
}


def read_wav(path):
    """Minimal WAV reader that handles both int16 (tag 1) and float32 (tag 3),
    which is exactly the case Python's `wave` module rejects."""
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    i, fmt, data = 12, None, None
    while i + 8 <= len(d):
        cid, sz = d[i:i + 4], struct.unpack("<I", d[i + 4:i + 8])[0]
        body = d[i + 8:i + 8 + sz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])       # tag ch rate br align bits
        elif cid == b"data":
            data = body
        i += 8 + sz + (sz & 1)                              # chunks are word-aligned
    if fmt is None or data is None:
        raise ValueError(f"{path}: missing fmt or data chunk")

    tag, ch, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(data, dtype="<f4").astype(np.float32)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f"{path}: unsupported format tag={tag} bits={bits}")
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)                  # downmix to mono
    return x, rate, tag, bits, ch


def resample_linear(x, src_sr, dst_sr):
    """Linear resample. Adequate for short speech prompts and avoids a scipy
    dependency; if a phrase ever needs real quality, resample upstream."""
    if src_sr == dst_sr:
        return x
    n_out = int(round(len(x) * dst_sr / src_sr))
    return np.interp(
        np.linspace(0.0, len(x) - 1, n_out, dtype=np.float64),
        np.arange(len(x), dtype=np.float64),
        x,
    ).astype(np.float32)


def trim_silence(x, sr, floor_db=TRIM_DB, pad_ms=25):
    """Trim leading/trailing near-silence, keeping a short pad so speech does
    not start abruptly (an abrupt start clicks through the DAC)."""
    if x.size == 0:
        return x
    thresh = 10.0 ** (floor_db / 20.0) * np.max(np.abs(x))
    loud = np.flatnonzero(np.abs(x) > thresh)
    if loud.size == 0:
        return x
    pad = int(sr * pad_ms / 1000)
    return x[max(0, loud[0] - pad): min(len(x), loud[-1] + pad + 1)]


def to_int16(x):
    """Peak-normalise to -1 dBFS then quantise. Consistent loudness across the
    bank matters more than absolute level: the PAM8403 has no volume control."""
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    if peak > 0:
        x = x * (10.0 ** (-1.0 / 20.0) / peak)
    return np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2")


def write_wav_int16(path, pcm, sr):
    n = pcm.nbytes
    hdr = (b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt "
           + struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 2, 2, 16)
           + b"data" + struct.pack("<I", n))
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit-header", action="store_true",
                    help=f"also write {HEADER}")
    ap.add_argument("--no-trim", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(SRC_DIR):
        sys.exit(f"error: {SRC_DIR}/ not found -- run from the repo root")
    os.makedirs(OUT_DIR, exist_ok=True)

    srcs = sorted(f for f in os.listdir(SRC_DIR) if f.lower().endswith(".wav"))
    if not srcs:
        sys.exit(f"error: no .wav files in {SRC_DIR}/")

    print(f"{'file':16s} {'in':>22s}   {'out':>18s}  {'dur':>6s} {'flash':>8s}")
    print("-" * 78)

    results, total, over = [], 0, []
    for fn in srcs:
        stem = os.path.splitext(fn)[0].lower()
        x, sr, tag, bits, ch = read_wav(os.path.join(SRC_DIR, fn))
        x = resample_linear(x, sr, TARGET_SR)
        if not args.no_trim:
            x = trim_silence(x, TARGET_SR)
        pcm = to_int16(x)

        dur = len(pcm) / TARGET_SR
        total += pcm.nbytes
        if dur > MAX_SEC:
            over.append((fn, dur))

        write_wav_int16(os.path.join(OUT_DIR, f"{stem}.wav"), pcm, TARGET_SR)
        name = NAME_MAP.get(stem)
        results.append((stem, name, pcm))

        tagname = {1: "int", 3: "float"}.get(tag, f"tag{tag}")
        print(f"{fn:16s} {f'{tagname}{bits} {ch}ch {sr}Hz':>22s} -> "
              f"{'int16 1ch 24000Hz':>18s}  {dur:5.2f}s {pcm.nbytes/1024:7.1f}K"
              + ("  ! over 2s" if dur > MAX_SEC else ""))

    print("-" * 78)
    print(f"{len(results)} phrase(s), {total/1024:.1f} KB of flash "
          f"(budget ~300 KB, WROVER has 4 MB)")

    missing = [k for k in NAME_MAP if k not in {r[0] for r in results}]
    if missing:
        print(f"\nnot yet recorded ({len(missing)}): {', '.join(sorted(missing))}")
        print("  -> blocked on picking the cloud TTS voice first, so the clone "
              "matches. See 00-TEAM-PLAN.md D6.")
    if over:
        for fn, d in over:
            print(f"\n! {fn} is {d:.2f}s, over the {MAX_SEC}s budget -- "
                  "re-record it shorter.")

    if args.emit_header:
        unnamed = [s for s, n, _ in results if n is None]
        if unnamed:
            print(f"\nerror: no NAME_MAP entry for: {', '.join(unnamed)}")
            print("       add them to NAME_MAP before emitting the header.")
            return 1
        with open(HEADER, "w", newline="\n") as f:
            f.write("// GENERATED by tools/wav_to_phrases.py -- DO NOT HAND-EDIT\n"
                    "// Regenerate after changing anything in Messages/.\n"
                    "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n"
                    f"#define PHRASE_SAMPLE_RATE {TARGET_SR}\n\n")
            # HAVE_PHRASE_<NAME> lets the firmware compile against the full
            # phrase set while only some are recorded. Without it there is no
            # way to test for a phrase's existence -- these are variables, not
            # macros, so #ifdef on the array name would not work.
            f.write("// Which phrases exist in this build. The state machine\n"
                    "// needs all of them; missing ones are logged, not fatal.\n")
            for _, name, _ in results:
                f.write(f"#define HAVE_PHRASE_{name} 1\n")
            f.write("\n")

            for stem, name, pcm in results:
                f.write(f"// {stem}.wav -- {len(pcm)/TARGET_SR:.2f}s, "
                        f"{pcm.nbytes/1024:.1f} KB\n"
                        f"const int16_t PHRASE_{name}[] PROGMEM = {{"
                        + ",".join(str(int(s)) for s in pcm)
                        + f"}};\nconst size_t PHRASE_{name}_LEN = {len(pcm)};\n\n")
        print(f"\nwrote {HEADER}  ({os.path.getsize(HEADER)/1024:.0f} KB source)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
