#!/usr/bin/env python3
"""
score.py -- run the eval set and print a score.

    python tools/eval/score.py --base-url http://127.0.0.1:8080/api/v1
    python tools/eval/score.py                     # real OpenRouter

Run it on every prompt change. IF THE SCORE DOES NOT MOVE, DO NOT SHIP THE
CHANGE (02-SOFTWARE.md section 5).

Two numbers come out, and they are deliberately kept separate:

  NUMBERS  exact-match on doses, dates, amounts, bus numbers. Machine-scored,
           and the one that matters -- a wrong dose is worse than clumsy
           phrasing. This should be 100%.
  CONTENT  substring checks on the words that must appear. A rough proxy.

The third measure, "would this be useful spoken aloud?", is a human judgement.
Set useful_spoken true/false by hand in expected.json; it is reported but never
computed.
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import requests                                            # noqa: E402
from reference_pipeline import (PROMPTS, clean, load_key,   # noqa: E402
                                vision)

HERE = os.path.dirname(os.path.abspath(__file__))


# A label reading "3 times a day" and one reading "three times a day" are the
# same fact, and the model correctly transcribes whichever is printed. Without
# this, every run drowns in false failures on spelled-out numbers.
WORD_NUMBERS = {
    "zero": 0, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6,
    "seven": 7, "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12,
    "thirteen": 13, "fourteen": 14, "fifteen": 15, "sixteen": 16,
    "seventeen": 17, "eighteen": 18, "nineteen": 19, "twenty": 20,
    "thirty": 30, "forty": 40, "fifty": 50, "sixty": 60, "seventy": 70,
    "eighty": 80, "ninety": 90, "hundred": 100, "thousand": 1000,
    # months, so "12 September" and "12/09" score the same
    "january": 1, "february": 2, "march": 3, "april": 4, "may": 5, "june": 6,
    "july": 7, "august": 8, "september": 9, "october": 10, "november": 11,
    "december": 12,
}


def numbers_in(text):
    """The set of numeric facts in the text, as canonical digit strings.
    Covers digit runs ('$184.00' -> 184, 00; '12/09' -> both parts) and
    number words, so digits and spelled-out forms compare equal."""
    found = {n.lstrip("0") or "0" for n in re.findall(r"\d+", text)}
    for word in re.findall(r"[a-z]+", text.lower()):
        if word in WORD_NUMBERS:
            found.add(str(WORD_NUMBERS[word]))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default=os.environ.get(
        "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1"))
    ap.add_argument("--expected", default=os.path.join(HERE, "expected.json"))
    ap.add_argument("--images", default=os.path.join(HERE, "images"))
    ap.add_argument("--only", help="run a single item id")
    args = ap.parse_args()

    key, key_src = load_key()
    if not key and "openrouter.ai" in args.base_url:
        sys.exit(f"No API key ({key_src}), and --base-url is the real API.")

    spec = json.load(open(args.expected, encoding="utf-8"))
    items = [i for i in spec["items"]
             if not args.only or i["id"] == args.only]
    if not items:
        sys.exit("no matching items")

    session = requests.Session()
    num_hit = num_tot = con_hit = con_tot = 0
    missing, rows = [], []

    for item in items:
        path = os.path.join(args.images, item["file"])
        if not os.path.exists(path):
            missing.append(item["file"])
            continue

        raw, _ = vision(session, args.base_url, key, path, item["mode"])
        text, _ = clean(raw)
        got = numbers_in(text)
        want = {str(n).lstrip("0") or "0" for n in item.get("numbers", [])}
        hit_n = want & got
        miss_n = want - got
        num_hit += len(hit_n)
        num_tot += len(want)

        low = text.lower()
        want_c = [c.lower() for c in item.get("must_contain", [])]
        hit_c = [c for c in want_c if c in low]
        con_hit += len(hit_c)
        con_tot += len(want_c)

        rows.append((item["id"], len(hit_n), len(want), len(hit_c),
                     len(want_c), sorted(miss_n),
                     [c for c in want_c if c not in low],
                     item.get("useful_spoken"), text))

    print()
    for (iid, nh, nt, ch, ct, miss_n, miss_c, useful, text) in rows:
        bad = miss_n or miss_c
        print(f"  {'FAIL' if bad else 'ok  '}  {iid:28s} "
              f"numbers {nh}/{nt}  content {ch}/{ct}"
              f"{'  useful=' + str(useful) if useful is not None else ''}")
        if miss_n:
            print(f"          MISSED NUMBERS: {', '.join(miss_n)}")
        if miss_c:
            print(f"          missing words : {', '.join(miss_c)}")
        if bad:
            print(f"          got: {text[:120]!r}")

    print("\n  " + "-" * 60)
    if num_tot:
        print(f"  NUMBERS  {num_hit}/{num_tot} "
              f"({100 * num_hit / num_tot:.0f}%)   <- must be 100%")
    if con_tot:
        print(f"  CONTENT  {con_hit}/{con_tot} "
              f"({100 * con_hit / con_tot:.0f}%)")
    judged = [r[7] for r in rows if r[7] is not None]
    print(f"  USEFUL   {sum(1 for j in judged if j)}/{len(judged)} judged "
          f"by hand ({len(rows) - len(judged)} unjudged)")
    print(f"  scored {len(rows)} of 20 target images")
    if missing:
        print(f"\n  {len(missing)} image(s) not shot yet: "
              f"{', '.join(missing[:6])}{' ...' if len(missing) > 6 else ''}")
    return 1 if (num_tot and num_hit < num_tot) else 0


if __name__ == "__main__":
    sys.exit(main())
