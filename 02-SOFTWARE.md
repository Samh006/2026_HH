# Software Brief — Talking Reader

**Owners: 2 software engineers + 1 computer engineer on firmware** · Read `00-TEAM-PLAN.md` first.

Two sub-tracks that meet at a mock server:

| Sub-track | Who | Owns |
|---|---|---|
| **Cloud & tooling** | 2 software engineers | Reference pipeline, prompts, eval set, mock server, phrase generation |
| **Firmware** | Computer engineer, pairing with 1 SWE | Camera, Wi-Fi/TLS, uploader, I²S playback, buttons, state machine |

The firmware track **never talks to OpenRouter during development**. It talks to the mock. That keeps firmware work deterministic, free, and possible with no internet — and it's our demo-day insurance.

---

## 1. Repo

```
talking-reader/
├── firmware/
│   ├── platformio.ini
│   ├── src/
│   │   ├── main.cpp        # state machine
│   │   ├── camera.cpp      # capture + JPEG config
│   │   ├── net.cpp         # streaming base64 POST   ← hardest file
│   │   ├── audio.cpp       # PCM → I²S
│   │   ├── phrases.h       # GENERATED — do not hand-edit
│   │   └── config.h        # 🚫 gitignored
│   └── include/config.example.h
├── tools/
│   ├── reference_pipeline.py
│   ├── mock_server.py
│   ├── make_phrases.py
│   └── eval/
│       ├── images/         # 20 real photos
│       └── expected.json
└── docs/decisions.md
```

**Use PlatformIO, not the Arduino IDE.** Library versions get pinned in `platformio.ini`, the project is diffable, and four people get identical builds. The Arduino IDE's global library folder will hand you a "works on my machine" bug you can't afford.

**`config.h` is gitignored from commit one.** Commit `config.example.h`. Someone otherwise pushes the API key to a public repo — it happens on nearly every hackathon team.

---

## 2. First three days

### Day 1 — everyone together

- PlatformIO installed on all machines
- Flash Freenove's `CameraWebServer` example, view the stream in a browser *(proves board, camera, USB, Wi-Fi in 20 minutes)*
- Everyone makes one `curl` to OpenRouter with an image and sees text come back
- Agree button map and modes; write them into `docs/decisions.md`

### Day 2 — split

**Cloud:** `reference_pipeline.py` end to end — a JPEG on disk becomes a playable WAV. Then `mock_server.py`.
**Firmware:** capture a JPEG to PSRAM, print its size over serial, plain-HTTP POST a *tiny hardcoded* payload to the mock. Don't touch base64 yet.

### Day 3 — integration checkpoint

**Firmware:** the streaming base64 uploader against the mock, with a real photo.
**Cloud:** start the eval set.

✅ *Done when: pressing reset makes canned text appear over serial, derived from a photo the board just took.*

**If day 3 slips, say so on day 3.** Everything downstream depends on this loop.

---

## 3. The mock server

Write this on day 1. Highest-leverage 40 lines in the project.

```python
# tools/mock_server.py   —   python mock_server.py
from flask import Flask, request, jsonify, Response
import time, wave

app = Flask(__name__)

CANNED = ("Amoxicillin 500 milligrams. Take one capsule three times a day "
          "with food. Complete the full course.")

@app.post("/api/v1/chat/completions")
def vision():
    img = request.get_json()["messages"][0]["content"][1]["image_url"]["url"]
    print(f"[mock] image received, {len(img)} chars of base64")
    time.sleep(2.5)                     # imitate real latency — do not remove
    return jsonify({"choices": [{"message": {"content": CANNED}}]})

@app.post("/api/v1/audio/speech")
def speech():
    print(f"[mock] TTS: {request.get_json()['input'][:60]}...")
    time.sleep(1.0)
    with wave.open("tools/sample_24k_mono.wav", "rb") as w:
        pcm = w.readframes(w.getnframes())
    return Response(pcm, mimetype="audio/pcm")

app.run(host="0.0.0.0", port=8080)      # 0.0.0.0 — the ESP32 must reach it
```

Firmware points at `http://<laptop-ip>:8080`. **Plain HTTP against the mock is deliberate** — get the whole request/response loop working before adding TLS, so when TLS breaks you know it's TLS. Add HTTPS in week 2 as an isolated step.

Keep the `time.sleep` calls. Without them you'll design the interaction around instant responses and discover the silence problem far too late.

---

## 4. The real API

Base URL `https://openrouter.ai/api/v1`, header `Authorization: Bearer sk-or-...`.

### Vision

```http
POST /api/v1/chat/completions
```
```json
{
  "model": "google/gemini-3-flash-preview",
  "max_tokens": 400,
  "messages": [{
    "role": "user",
    "content": [
      { "type": "text", "text": "<prompt>" },
      { "type": "image_url",
        "image_url": { "url": "data:image/jpeg;base64,/9j/4AAQ..." } }
    ]
  }]
}
```

Text part **before** the image — the docs are explicit that this parses better. Response is the standard OpenAI shape; you want `choices[0].message.content`.

### Text to speech

```http
POST /api/v1/audio/speech
```
```json
{
  "model": "openai/gpt-4o-mini-tts-2025-12-15",
  "input": "Amoxicillin 500 milligrams...",
  "voice": "alloy",
  "response_format": "pcm",
  "speed": 1.0
}
```

Returns **raw audio bytes**, not JSON.

🔑 **`pcm`, never `mp3`.** Raw samples go straight into the I²S DMA buffer — no decoder, no library, no CPU. MP3 means dragging in `ESP32-audioI2S` and fighting its HTTP layer, which wants to fetch URLs rather than consume a POST response. This one flag is worth several days.

**Confirm the sample rate on day 1** — capture a response to a file on your laptop and inspect the header. OpenAI-family TTS is typically 24 kHz 16-bit mono. Get this wrong and everything plays chipmunked. Tell the firmware track the number.

TTS bills **per input character**, so short responses are cheaper *and* better UX.

---

## 5. Prompts

The highest-leverage code in the project. A raw model says *"This image shows a medicine bottle. The label reads:..."* every single time — maddening on the fortieth use.

**Mode A — Read** (button A, short press)

> Transcribe all text visible in this image, exactly as written. Preserve the reading order a sighted person would use. Output ONLY the transcribed text — no preamble, no description, no commentary, no markdown. If the image contains no legible text, output exactly: NOTEXT

**Mode B — Describe** (button B)

> Describe what is in front of the user in one or two short sentences, as if speaking to a person with low vision who is holding this camera. Lead with the most important object. Mention text only if it identifies the object. No preamble, no markdown, under 40 words.

**Mode C — Summarise** (button A, long press)

> This is a photo of a document. In under 60 words, tell the reader what kind of document it is and the single most important piece of information in it — an amount, a date, a deadline, a dose, or an action required. Plain speech, no preamble, no markdown.

### Rules for all of them

- **"No markdown" is load-bearing** — asterisks and hashes get read aloud as noise
- Cap `max_tokens`. Runaway output is expensive and unlistenable.
- Handle `NOTEXT` **on the device** with a pre-recorded phrase. Never send "there is no text" to TTS — it costs money and sounds like a malfunction.
- Strip any leading "Sure," / "Here is" on device as belt-and-braces
- **Numbers are the failure case people actually care about.** Test doses, expiry dates, bus numbers and dollar amounts specifically.

### Eval set — build it in week 1

20 photos in `tools/eval/images/`, ground truth in `expected.json`. Shoot **real objects**, not printouts of clean paragraphs: medicine bottles, glossy laminated menus, curved packaging, low-contrast utility bills, handwriting, a bus timetable.

Score = exact-match on the numbers, plus a human "would this be useful spoken aloud?" judgement. Run it on every prompt change. **If the score doesn't move, don't ship the change.**

---

## 6. Firmware — the three hard bits

Everything else is tutorial-level. Budget your weeks around these.

### 6.1 TLS memory

`WiFiClientSecure` wants ~40–50 KB of *internal* heap for the handshake, and it does **not** use PSRAM by default. Combined with the camera framebuffer, this is where crashes live.

- Camera framebuffer in PSRAM: `fb_location = CAMERA_FB_IN_PSRAM`, `fb_count = 1`
- **One TLS connection at a time.** Close the vision connection before opening the TTS one.
- `client.setInsecure()` is acceptable for a hackathon and avoids certificate-expiry pain on demo day. Name it as a known limitation in the writeup rather than hoping nobody asks.
- Log `ESP.getFreeHeap()` before and after every phase from day one.

### 6.2 Streaming base64

A 60 KB JPEG becomes ~80 KB of base64. Building that as an Arduino `String` means peak allocations north of 200 KB and reallocation churn — it works on the bench and fails during the demo.

Cleanest approach: wrap the framebuffer in a `Stream` that base64-encodes on demand, and hand it to `HTTPClient`. You get TLS and **chunked-response decoding** for free, which matters — OpenRouter may reply chunked, and hand-rolling that parser is a bad afternoon.

```cpp
static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

class B64Stream : public Stream {
  const uint8_t *src; size_t n, i = 0;
  char out[4]; uint8_t len = 0, pos = 0;

  bool fill() {
    if (i >= n) return false;
    size_t rem = n - i;
    uint32_t v = (uint32_t)src[i] << 16
               | (rem > 1 ? (uint32_t)src[i+1] << 8 : 0)
               | (rem > 2 ? (uint32_t)src[i+2]      : 0);
    out[0] = B64[(v >> 18) & 63];
    out[1] = B64[(v >> 12) & 63];
    out[2] = rem > 1 ? B64[(v >> 6) & 63] : '=';
    out[3] = rem > 2 ? B64[ v       & 63] : '=';
    i += (rem > 3 ? 3 : rem);
    len = 4; pos = 0;
    return true;
  }

public:
  B64Stream(const uint8_t *d, size_t sz) : src(d), n(sz) {}
  size_t encodedLength() const { return 4 * ((n + 2) / 3); }   // for Content-Length

  int available() override { return (pos < len || i < n) ? 1 : 0; }
  int read() override {
    if (pos >= len && !fill()) return -1;
    return (uint8_t)out[pos++];
  }
  int peek() override {
    if (pos >= len && !fill()) return -1;
    return (uint8_t)out[pos];
  }
  size_t write(uint8_t) override { return 0; }
};
```

Content-Length is deterministic: `prefixLen + b64.encodedLength() + suffixLen`. Send the JSON prefix, the stream, then the suffix.

Peak extra RAM: about a kilobyte. **This is the most valuable hour of engineering in the project.**

### 6.3 PCM straight to I²S

Read the `/audio/speech` body in chunks and push each one into `i2s_write()` as it arrives. Don't buffer the whole utterance — 20 seconds at 24 kHz/16-bit mono is ~960 KB and delays first audio for no benefit.

**Keep a one-byte carry** across chunk boundaries. A chunk ending on an odd byte splits a 16-bit sample, and you'll get a periodic click you'll spend an evening chasing.

```cpp
// hardware pin map — see 01-HARDWARE.md
i2s_pin_config_t pins = {
  .mck_io_num   = I2S_PIN_NO_CHANGE,   // GPIO 0 only if the DAC needs MCLK
  .bck_io_num   = 32,
  .ws_io_num    = 33,
  .data_out_num = 14,
  .data_in_num  = I2S_PIN_NO_CHANGE
};

i2s_config_t cfg = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
  .sample_rate = 24000,                          // CONFIRM against a real response
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,   // TTS is mono
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .dma_buf_count = 8,
  .dma_buf_len = 512
};
```

⚠️ Two things to verify with the hardware track on day 2: **whether the ES7148 needs MCLK on SCK**, and **which channel the speaker is wired to**. If you hear silence but the DAC is clocking, try `I2S_CHANNEL_FMT_RIGHT_LEFT` and duplicate each sample.

*(Arduino-ESP32 core 3.x replaces this API with the `I2SClass` wrapper — check which core `platformio.ini` pins before copying. Freenove ships examples for both.)*

---

## 7. State machine

```
IDLE ──button──▶ CAPTURE ──▶ UPLOAD ──▶ WAIT_TEXT ──▶ UPLOAD_TTS ──▶ SPEAKING
  ▲                                                                      │
  └──────────────── any button, or end of audio ─────────────────────────┘
                              │
   any failure ──▶ SPEAK_PHRASE(error) ──▶ IDLE
```

**Non-negotiable behaviours:**

- Any button press during `SPEAKING` stops playback immediately. Users must never feel trapped by their own device.
- Button presses during `CAPTURE`/`UPLOAD` are **ignored, not queued**. A queued second press means a second paid API call and a confusing double-read.
- Every failure path speaks a pre-recorded phrase. **Silence is a bug.**

### Filling the wait

| Time | Event |
|---|---|
| 0.0 s | shutter click — sharp, immediate, confirms the press physically |
| 0.2 s | "Reading" (pre-recorded, from flash) |
| 0.5 s | soft tick, once per second, quiet |
| ~4 s | text returns, tick stops |
| ~6 s | speech begins |

---

## 8. Offline phrases

Fifteen or so fixed strings baked into flash as PCM: *Ready · Reading · Describing · Nothing found, try moving closer · No internet · Connecting · Battery low · Something went wrong · Repeating.*

Generate with pocket-tts on a laptop. **Use its voice cloning**: sample the OpenRouter TTS voice you picked, clone it, and generate the phrases in that voice — so offline phrases and cloud speech are one voice, not two. About an hour's work, and it's what makes the device feel finished rather than assembled.

```python
# tools/make_phrases.py
from pocket_tts import TTSModel
import numpy as np

PHRASES = {
    "READY":       "Ready",
    "READING":     "Reading",
    "NO_TEXT":     "Nothing found. Try moving closer.",
    "NO_INTERNET": "No internet connection.",
    "BATT_LOW":    "Battery low.",
    "ERROR":       "Something went wrong.",
}

m = TTSModel.load_model()
voice = m.get_state_for_audio_prompt("cloud_voice_sample.wav")   # clone it

with open("firmware/src/phrases.h", "w") as f:
    f.write("// GENERATED by make_phrases.py — do not edit\n#pragma once\n\n")
    for name, text in PHRASES.items():
        pcm = np.clip(m.generate_audio(voice, text) * 32767, -32768, 32767)
        pcm = pcm.astype("<i2")                       # 16-bit LE, resample to 24 kHz
        f.write(f"const int16_t PHRASE_{name}[] PROGMEM = {{"
                + ",".join(str(s) for s in pcm) + f"}};\n"
                + f"const size_t PHRASE_{name}_LEN = {len(pcm)};\n\n")
```

Watch the total size — the WROVER has 4 MB flash. At 24 kHz 16-bit that's 48 KB per second, so **keep every phrase under two seconds** and the whole bank stays around 300 KB. If it grows, drop the phrase bank to 16 kHz and resample on playback.

---

## 9. Measure these

Into `docs/measurements.md` in week 2 — real numbers beat estimates in a writeup.

| | |
|---|---|
| Time to first spoken word | target < 6 s |
| JPEG size at each frame size | tune `FRAMESIZE_SVGA` (800×600) vs UXGA |
| Cost per press | OpenRouter's activity log shows per-request cost |
| Eval score across the 20 photos | |
| Free heap at each phase | |
| Failure rate over 50 presses | the number judges actually care about |

**Biggest latency lever is JPEG size.** Try SVGA at quality 12 before reaching for UXGA — a model reads a medicine label from 800×600 perfectly well, and you halve the upload. Measure the accuracy/latency tradeoff in week 2; don't guess.

---

## 10. Week by week

**Week 1 — the loop, mocked.** Reference pipeline, mock server, prompts finalised on 20 photos, phrases generated. Firmware: capture → base64 → POST → PCM → speaker, over plain HTTP.
✅ *Button press produces speech from the speaker.*

**Week 2 — the loop, real.** Swap in OpenRouter. Add TLS as an isolated step. Three modes. Measurements taken.
✅ *Reads a real medicine label, on Wi-Fi, in under 6 seconds.*

**Week 3 — make it robust.** Every failure path speaks. Cache and repeat. Hotspot fallback. Support the hardware track through assembly, and watch the user tests — you'll learn more in that hour than in a week of coding.
✅ *A stranger uses it unaided.*

**Week 4 — freeze day 22.** Fixes only. Rehearse the demo three times.

---

## 11. Gotchas

1. **Chunked responses.** Use `HTTPClient`'s stream rather than raw sockets, or you're writing a chunked-transfer parser at midnight.
2. **`String` concatenation on ESP32.** Fragments the heap. Use `char[]` and streams in `net.cpp`.
3. **Sample rate mismatch.** Chipmunk audio means your I²S rate doesn't match the TTS output. Confirm the real number on day 1.
4. **Odd-byte chunk boundaries.** Keep the one-byte carry, or hear clicks.
5. **Captive portals.** Venue Wi-Fi with a login page will silently break TLS. Hardcode two phone hotspots with fallback and test on the real one in week 2.
6. **Watchdog resets during long uploads.** Feed it, or run the network work on its own task.
7. **The mock is also the demo fallback.** Keep it working all four weeks. If the venue Wi-Fi dies, you point at a laptop and still demo.

---

*Hardware pin map, I²S wiring and the MCLK question → `01-HARDWARE.md`.*
