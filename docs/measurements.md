# Measurements

Real numbers beat estimates in a writeup, and judges like measured numbers.
Fill these in as they are taken; note the date and the conditions.

## Software (target: week 2)

| Measurement | Target | Value | Date | Conditions |
|---|---|---|---|---|
| Time to first spoken word | < 6 s | **~5.05 s** (cloud legs only) | 2026-08-31 | vision 3.45 s + TTS first byte 1.60 s. Excludes capture, upload from the device, and I2S start |
| Vision call latency | | **3.45 s** | 2026-08-31 | `gemini-3-flash-preview`, 16.8 KB SVGA q12 JPEG, 1177 tokens |
| Vision latency, model comparison | | 3.5-flash-lite **1.54 s** / 3.1-flash-lite 1.69 s / 3-flash-preview 2.97 s / 2.5-flash-lite 3.40 s | 2026-08-31 | same image, `compare_models.py` |
| Cost per 1000 presses (vision only) | | 3.5-flash-lite $0.45 / 3.1-flash-lite $0.35 / 3-flash-preview $0.71 / 2.5-flash-lite $0.13 | 2026-08-31 | live OpenRouter pricing |
| TTS first audio byte | | **1.60 s** | 2026-08-31 | `gpt-audio-mini` streaming; full stream done at 2.28 s |
| JPEG size at SVGA q12 | | ~12 KB (synthetic test image) | 2026-08-31 | not a camera capture -- replace with a real one |
| JPEG size at UXGA | | | | |
| Cost per press | | | | OpenRouter activity log |
| Eval score -- NUMBERS | 100% | | | needs the 20 photos |
| Eval score -- CONTENT | | | | |
| Free heap after camera init | | | | |
| Free heap during TLS handshake | | | | the number that matters -- section 6.1 |
| Free heap during playback | | | | |
| Failure rate over 50 presses | | | | the number judges actually care about |

### Server round trip — measured 2026-09-18, live server on ICP-WiFi

Kristian's ASP.NET server at `192.168.6.22:5148`, POSTed from the laptop with
`curl` so the device is not in the loop. JPEG in, last audio byte out.

| Image | Status | Time | Body | Speech |
|---|---|---|---|---|
| ceiling vent, no text | 422 | **10.19 s** | 14 B (`No text found.`) | — |
| printed box, small text | 200 | **3.93 s** | 124 KB | 2.58 s |
| dense text | 200 | **9.95 s** | 585 KB | 12.19 s |
| same box, repeat request | 200 | 2.31 s | — | — |

Response format, by header parse: **PCM int16, 1 channel, 24000 Hz, 16-bit**,
`Content-Type: audio/wav`, `Content-Length` present (not chunked). Matches
`SERVER_CONTRACT.md` §3 exactly.

Two numbers that changed the firmware:

- **9.95 s** — past HTTPClient's 5 s default, which `client.cpp` was not
  overriding. Every long press would have failed and blamed the network.
- **585 KB** — the reply was being buffered in a `std::vector` in internal
  heap, where boot leaves **318 KB free / 278 KB largest contiguous** (measured
  same day). Long readings could not have fitted. It goes to PSRAM now.

### Device — measured 2026-09-18 at boot

| Measurement | Value |
|---|---|
| Free heap at boot | 318,684 B |
| Min free heap at boot | 313,536 B |
| Largest contiguous alloc at boot | 278,516 B |
| Free PSRAM at boot | 8,386,019 B |
| Repeat cache (PSRAM, once at boot) | 1,440,064 B = 30 s @ 24 kHz 16-bit mono |
| Build | RAM 17.0%, flash 36.0% (1,133,808 B) |
| `ICP-WiFi` as the board sees it | 2.4 GHz, ch 1 −63 dBm and ch 11 −64 dBm, `wpa2-psk` |

### Full press path — measured 2026-09-18 on the device, three presses

Real presses, real labels, `ICP-WiFi`, board at `192.168.6.42` (RSSI −73),
server at `192.168.6.22:5148`.

| | Press 1 | Press 2 | Press 3 |
|---|---|---|---|
| Camera capture | 79,475 B / **558 ms** (cold, incl. sensor init) | 94,454 B / **283 ms** | 83,043 B / **358 ms** |
| `send_jpeg` | **3,231 ms**, 0 B down | **5,268 ms**, 78,774 B down | **16,566 ms**, 608,582 B down |
| Server said | **422** | 200 | 200 |
| Played | `no_text` phrase (1.43 s) | 39,364 samples, 1.64 s | 304,268 samples, **12.68 s** |
| **Press to first word** | — | **~5.6 s** | **~16.9 s** |

Playback is exact both times — `[play] ok` with every declared sample
accounted for, from a `RIFF: 24000 Hz 16-bit int 1ch` body.

**Against the < 6 s target: press 2 makes it, press 3 misses it by 11 s.** The
device's own share is tiny — 283–358 ms of capture — so this is entirely the
server's vision-plus-synthesis time, and it scales with how much text there is
to speak. A 12.68 s reading costs 16.6 s to obtain. Nothing on the device can
improve that; it needs either streaming from the server or a shorter reading.

⚠️ **And the device is silent for all of it**, because `reading` is one of the
7 unrecorded phrases. 17 seconds of silence after a shutter click is
indistinguishable from a broken device to someone who cannot see it. **This is
now the highest-value fix in the project** — it needs no server work, just the
Kokoro voice decision.

Two of today's fixes are vindicated by press 3 specifically:

| Measurement | Value | Why it matters |
|---|---|---|
| Longest round trip | **16,566 ms** | HTTPClient's default timeout is **5 s**, and `client.cpp` was not overriding it. This press would have failed, and blamed the network |
| Largest reply | **608,582 B** | The reply used to be buffered in a `std::vector` in internal heap, where the largest contiguous block measured **237,556 B**. This press would have thrown `bad_alloc` |

Heap across three presses, i.e. **no leak** (D26's `free()` is behaving):

| After | Free heap | Largest | Free PSRAM |
|---|---|---|---|
| boot | 271,592 | 262,132 | 6,934,743 |
| press 1 | 250,352 | 237,556 | 5,675,175 |
| press 2 | 250,128 | 237,556 | 5,675,175 |
| press 3 | 249,904 | 237,556 | 5,675,175 |

PSRAM returns to exactly 5,675,175 B after every press. The ~1.26 MB standing
difference from boot is the camera's two QXGA framebuffers; the 1.44 MB replay
cache is inside the boot figure.

**Camera frames are 79–94 KB at QXGA on real labels** — lower than D28's 137 KB,
which was a different (larger, busier) scene. Frame size is scene-dependent;
budget 150–200 KB and do not treat any single figure as the number.

**Still unmeasured:** button B long (the PSRAM replay path has never been
exercised), and free heap *during* playback rather than side to side of it.

### Confirmed already

| Fact | Value | How |
|---|---|---|
| Phrase WAV sample rate | 24 kHz mono | header inspection, all 3 files |
| Phrase WAV source format | 32-bit IEEE float (converted to int16 -- D7) | header inspection |
| Phrase bank size, 3 phrases | 178.7 KB of 4 MB flash | `wav_to_phrases.py` |
| Phrase durations after trim | ready 0.50 s, no_text 1.43 s, no_internet 1.88 s | `wav_to_phrases.py` |
| pocket-tts output format | 24 kHz mono, mimi frame rate 12.5 Hz | `config/english.yaml`, confirmed at runtime |
| pocket-tts token ceiling | 50 tokens = 4.0 s hard max per phrase | 12.5 frames/s x 50 |
| Voice cloning available? | **No** -- gated model, fell back to `pocket-tts-without-voice-cloning` (26 fixed voices) | verified 2026-08-31 |
| TTS sample rate from the real API | **24 kHz, 16-bit, mono, headerless** -- CONFIRMED | duration analysis, 2026-08-31 |
| TTS transport | SSE base64 deltas on `/chat/completions`, NOT a raw body (D14) | live API, 2026-08-31 |
| Vision accuracy, CLEAN synthetic label | 5/5 numbers, 3/3 words -- all four candidate models | live API, 2026-08-31 |
| Vision accuracy, HARD label (low contrast, curved, blurred) | **2/6 to 3/6 numbers -- every model, with confident hallucination** | live API, 2026-08-31 |
| Uncertainty-aware prompt on the hard label | 6 UNCLEAR markers emitted instead of silent guessing (D17) | live API, 2026-08-31 |
| Audio payload for a 73-char utterance | 232,800 bytes over 13 SSE chunks | live API, 2026-08-31 |

Generated-phrase durations in the `alba` catalog voice, trimmed (2026-08-31),
for sizing the bank. Generation is stochastic, so these move slightly per run:

| Phrase | Duration | Flash |
|---|---|---|
| "Reading" | 0.71 s | 33.2 KB |
| "Battery low." | 0.98 s | 45.9 KB |
| "Nothing found. Try moving closer." | 2.48 s | 116.2 KB — **over the 2 s budget** |

Extrapolating all 9 phrases at ~1.1 s average gives roughly **480–500 KB**, not
the ~300 KB the brief estimated (which assumed ~0.7 s average). Still
comfortable on 4 MB flash with `huge_app.csv`, but worth knowing before adding
more phrases. Shortening "Nothing found. Try moving closer." to "Nothing found.
Move closer." would bring the worst offender inside budget.

## Hardware (owner: mechatronics -- see 01-HARDWARE.md section 5)

| Measurement | Value |
|---|---|
| Idle current (camera on, Wi-Fi idle) | |
| Peak current during Wi-Fi transmit | |
| Current during audio playback | |
| Pack voltage -- fresh | |
| Pack voltage -- at brownout | |
| Runtime to brownout | |
| SPL at 0.5 m (want >= 75 dB) | |
| Assembled weight | |
