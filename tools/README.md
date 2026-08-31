# tools/ — cloud & tooling track

Run order on a fresh clone. Everything here works with no API key and no
internet except where marked.

```
pip install flask requests numpy pillow
```

### 1. Convert the phrase WAVs — do this first

```
python tools/wav_to_phrases.py --emit-header
```

The recorded WAVs in `Messages/` are 32-bit float; the device needs 16-bit
signed PCM (D7). This writes `tools/phrases_pcm16/` and generates
`firmware/src/phrases.h`. Both are gitignored — regenerate, don't commit.
The mock server reads its audio from `phrases_pcm16/`, so run this before it.

### 2. Start the mock server — what firmware develops against

```
python tools/mock_server.py
```

Plain HTTP, port 8080, deliberately (D10). Point firmware at
`http://<laptop-LAN-ip>:8080/api/v1` — **not** `127.0.0.1`, the ESP32 cannot
reach that. The startup banner prints the LAN IP.

Failure modes, for testing the state machine's error paths:

```
curl -X POST http://localhost:8080/mock/scenario -d scenario=notext
#   ok | notext | http500 | timeout | garbage | empty
curl http://localhost:8080/mock/status
```

Every uploaded photo is base64-validated (SOI/EOI/padding — this catches a
truncated streaming encoder immediately) and saved to `tools/captures/`, which
is a convenient source for the eval set.

### 3. Check the mock still works

```
python tools/test_mock.py          # needs the mock running
```

15 assertions covering the happy path, mode routing, upload validation and the
failure scenarios. Run it after any change to the mock, and before telling the
firmware track it's ready.

### 4. Reference pipeline — the ground truth firmware imitates

```
# against the mock, free, no key
python tools/reference_pipeline.py photo.jpg --base-url http://127.0.0.1:8080/api/v1

# against real OpenRouter
set OPENROUTER_API_KEY=sk-or-v1-...
python tools/reference_pipeline.py photo.jpg --mode read --probe-rate
```

`--mode read|describe|summarise`, `--text-only` to skip TTS while tuning
prompts (free), `--probe-rate` to confirm the real TTS sample rate. If the
device disagrees with this script, the device is wrong.

### 5. Eval set

```
python tools/eval/score.py --base-url http://127.0.0.1:8080/api/v1
```

Photos in `tools/eval/images/`, ground truth in `expected.json` — 3 items
templated, 17 still to shoot. Real objects, not printouts: medicine bottles,
glossy menus, curved packaging, low-contrast bills, handwriting, a timetable.
Run on every prompt change; if the score doesn't move, don't ship the change.

### 6. Phrase generation — installed and working, but a decision is pending

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
That puts D6 ("one voice throughout") in question — see the open item in
`docs/decisions.md`. The script explains both routes if you hit it.

Two things to know:

- **Clone samples must be int16.** pocket-tts reads voice prompts through
  Python's `wave` module, which cannot open the float32 files in `Messages/`.
  Use `tools/phrases_pcm16/` instead (same D7 bug, third appearance).
- **Always `--out-dir` somewhere scratch first.** Without it, generation writes
  straight into `Messages/` in whatever voice you passed. Listen before
  committing to a voice; `phrases.h` is only regenerated when writing to
  `Messages/`.

Generation is stochastic — the same phrase re-renders a little longer or
shorter each run. 6 of 9 phrases are still unrecorded.
