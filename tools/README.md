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

### 6. Phrase generation — blocked, don't run yet

```
python tools/make_phrases.py --voice-sample tools/cloud_voice_sample.wav
```

Needs `pip install pocket-tts`, and needs the cloud TTS voice chosen first
(D13) — the point is cloning the voice we actually ship. Generating before the
voice is picked means doing it twice. 6 of 9 phrases are still unrecorded.
