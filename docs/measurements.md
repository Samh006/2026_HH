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
