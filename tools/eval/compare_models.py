#!/usr/bin/env python3
"""
compare_models.py -- score several vision models on the same image(s).

    python tools/eval/compare_models.py <image.jpg> --numbers 500 21 09 2027
    python tools/eval/compare_models.py --all-eval        # whole eval set

Model choice is a latency/cost/accuracy tradeoff, and the brief is explicit
that we measure it rather than guess. Cheaper "lite" models are tempting --
input tokens dominate our bill because we send a big image and get a short
string back -- but a missed digit on a medicine label is the failure mode that
actually matters, so accuracy gates the decision, not price.

Cost is computed from OpenRouter's live pricing so it stays honest.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import requests                                             # noqa: E402
from reference_pipeline import clean, load_key, vision       # noqa: E402
from score import numbers_in                                 # noqa: E402

CANDIDATES = [
    "google/gemini-3-flash-preview",
    "google/gemini-3.5-flash-lite",
    "google/gemini-3.1-flash-lite",
    "google/gemini-2.5-flash-lite",
]


def pricing(session, key):
    r = session.get("https://openrouter.ai/api/v1/models",
                    headers={"Authorization": f"Bearer {key}"}, timeout=60)
    out = {}
    for m in r.json()["data"]:
        p = m.get("pricing") or {}
        try:
            out[m["id"]] = (float(p.get("prompt") or 0),
                            float(p.get("completion") or 0))
        except (TypeError, ValueError):
            pass
    return out


def run_one(session, key, model, image, mode, numbers, words):
    """Returns a result dict. Patches reference_pipeline's module-level model
    so the request body stays byte-identical to production."""
    import reference_pipeline as rp
    prev = rp.VISION_MODEL
    rp.VISION_MODEL = model
    try:
        t0 = time.time()
        raw, _ = vision(session, "https://openrouter.ai/api/v1", key,
                        image, mode)
        dt = time.time() - t0
    except SystemExit as exc:
        return {"model": model, "error": str(exc)}
    finally:
        rp.VISION_MODEL = prev

    text, notes = clean(raw)
    got = numbers_in(text)
    want = {str(n).lstrip("0") or "0" for n in numbers}
    low = text.lower()
    return {
        "model": model, "latency": dt, "text": text, "notes": notes,
        "num_hit": len(want & got), "num_tot": len(want),
        "missed": sorted(want - got),
        "word_hit": sum(1 for w in words if w.lower() in low),
        "word_tot": len(words),
        "chars": len(text),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--mode", default="read",
                    choices=["read", "describe", "summarise"])
    ap.add_argument("--numbers", nargs="*", default=[],
                    help="digits that MUST appear (exact match)")
    ap.add_argument("--words", nargs="*", default=[],
                    help="words that must appear (case-insensitive)")
    ap.add_argument("--models", nargs="*", default=CANDIDATES)
    ap.add_argument("--repeat", type=int, default=1,
                    help="runs per model; latency varies, accuracy should not")
    args = ap.parse_args()

    key, src = load_key()
    if not key:
        sys.exit(f"no API key ({src})")
    session = requests.Session()
    prices = pricing(session, key)

    rows = []
    for model in args.models:
        for _ in range(args.repeat):
            print(f"\n=== {model}")
            r = run_one(session, key, model, args.image, args.mode,
                        args.numbers, args.words)
            if "error" in r:
                print(f"  ERROR {r['error'][:160]}")
                rows.append(r)
                continue
            print(f"  text: {r['text'][:160]!r}")
            if r["missed"]:
                print(f"  MISSED NUMBERS: {', '.join(r['missed'])}")
            for n in r["notes"]:
                print(f"  ! {n}")
            rows.append(r)

    print("\n" + "=" * 96)
    print(f"{'model':34s} {'numbers':>8s} {'words':>7s} {'latency':>8s} "
          f"{'$/1k presses':>13s}  verdict")
    print("-" * 96)
    # ~1200 prompt tokens for an SVGA JPEG + prompt; completion measured
    for r in rows:
        if "error" in r:
            print(f"{r['model']:34s} {'-':>8s} {'-':>7s} {'-':>8s} "
                  f"{'-':>13s}  ERROR")
            continue
        pin, pout = prices.get(r["model"], (0, 0))
        cost1k = (1200 * pin + (r["chars"] / 4) * pout) * 1000
        exact = r["num_tot"] and r["num_hit"] == r["num_tot"]
        verdict = "usable" if exact or not r["num_tot"] else "DROPS DIGITS"
        print(f"{r['model']:34s} {r['num_hit']}/{r['num_tot']:<6d} "
              f"{r['word_hit']}/{r['word_tot']:<5d} {r['latency']:7.2f}s "
              f"${cost1k:12.2f}  {verdict}")
    print("\n  Numbers must be exact. A cheaper model that drops a dose digit\n"
          "  is not cheaper, it is wrong. Confirm on the full 20-photo eval\n"
          "  set before switching -- one synthetic image proves very little.")


if __name__ == "__main__":
    sys.exit(main())
