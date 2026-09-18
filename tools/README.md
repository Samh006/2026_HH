# tools/ — cloud & tooling track

Run order on a fresh clone.

```
pip install requests numpy
```

> **Changed 18 Sep (D29).** The mock server is gone — `mock_server.py`,
> `test_mock.py` and `mock_tts_sample.wav` were deleted when the firmware
> collapsed to a single backend. Two consequences worth knowing before you
> look for them:
>
> - There is **no offline path any more**. Everything here that talks to a
>   model needs a real `OPENROUTER_API_KEY`, and the device needs Kristian's
>   server running. Nothing in this directory is free except the WAV
>   conversion and phrase generation.
> - `restest` no longer deposits its sweep frames in `tools/captures/` —
>   `/mock/upload` was what put them there. The existing files stay; they are
>   still the handiest test images we have.

### 1. Convert the phrase WAVs — do this first

```
python tools/wav_to_phrases.py --emit-header
```

The recorded WAVs in `Messages/` are 32-bit float; the device needs 16-bit
signed PCM (D7). This writes `tools/phrases_pcm16/` and generates
`firmware/src/phrases.h`. Both are gitignored — regenerate, don't commit.

### 2. Keep the config template in step — do this after touching `config.h`

```
python tools/sync_config_template.py            # regenerate the template
python tools/sync_config_template.py --check    # verify only, exits 1 if stale
```

`firmware/src/config.h` is gitignored and per-machine, so it is the one file
where deleting a setting leaves no diff. On 17 Sep nine macros were dropped
from it while the sources still used them; the build failed, the previous image
stayed on the board, and the device looked like it had dead buttons. A session
went into finding that. This script generates
`firmware/include/config.example.h` from the live file — with credentials
scrubbed — so the committed record cannot lag behind. See D30.

### 3. Reference pipeline — the prompt the server has to use

```
set OPENROUTER_API_KEY=sk-or-v1-...
python tools/reference_pipeline.py photo.jpg --mode read --out out.wav
```

`--mode read|describe|summarise`, `--text-only` to skip TTS while tuning
prompts, `--probe-rate` to re-check the TTS sample rate if a model changes (it
is confirmed at 24 kHz — D15).

**What this is still for: `PROMPTS`.** The vision prompt in this file is the
one Kristian's server needs to send, and `docs/SERVER_CONTRACT.md` points at
it as the live copy. Two couplings must not break — the exact token `NOTEXT`
(the server turns it into HTTP 422 and sends no audio) and the exact tokens
`UNCLEAR` / `[?]` (D17's safety warning).

It is **no longer "the audio path to port"**. It used to be the only working
implementation of the D14 SSE-audio shape, for `speech.cpp` to copy.
`speech.cpp` is deleted and the device does no TTS of its own, so that half is
reference material now, not a specification.

### 4. Eval set

```
set OPENROUTER_API_KEY=sk-or-v1-...
python tools/eval/score.py
```

Photos in `tools/eval/images/`, ground truth in `expected.json` — 3 items
templated, 17 still to shoot. Real objects, not printouts: medicine bottles,
glossy menus, curved packaging, low-contrast bills, handwriting, a timetable.
Run on every prompt change; if the score doesn't move, don't ship the change.

Note this scores the **OpenRouter** API directly, not our server — it speaks
`/chat/completions`, and Kristian's server speaks `/api/tts/fromimage` and
returns audio rather than text. So it measures the prompt and the model, which
is what it was for, but it no longer measures the thing the device actually
talks to. Eval photos are still **0 of 10** shot.

### 5. Phrase generation — installed and working, but a decision is pending

`pocket-tts` 3.0.2 is installed. Two voice paths:

```
# catalog voice -- works now, no gating
python tools/make_phrases.py --voice alba --out-dir /tmp/try --only reading

# clone the cloud voice -- needs the GATED kyutai/pocket-tts weights
python tools/make_phrases.py --voice-sample tools/cloud_voice_sample.wav
```

**Voice cloning is gated.** `kyutai/pocket-tts` needs its terms accepted on
Hugging Face plus a local login; without that it silently falls back to
`pocket-tts-without-voice-cloning`, which has 26 fixed voices and no cloning.

That question may have dissolved: `docs/TODAY.md` (8 Sep) records that the
server synthesises with **Kokoro**, so the phrases should be generated in a
Kokoro voice to match, and the remaining action is to ask Kristian which one.
Until that is settled, **7 of 10 phrases are unrecorded** and the device is
genuinely silent on those failure paths — which breaks the one rule in
`CLAUDE.md` that is not negotiable. Boot prints exactly which are missing:

```
[phr ] missing: reading describing connecting batt_low error repeating uncertain   (7 of 10)
```

Two things to know:

- **Clone samples must be int16.** pocket-tts reads voice prompts through
  Python's `wave` module, which cannot open the float32 files in `Messages/`.
  Use `tools/phrases_pcm16/` instead (same D7 bug, third appearance).
- **Always `--out-dir` somewhere scratch first.** Without it, generation writes
  straight into `Messages/` in whatever voice you passed. Listen before
  committing to a voice; `phrases.h` is only regenerated when writing to
  `Messages/`.

Generation is stochastic — the same phrase re-renders a little longer or
shorter each run.
