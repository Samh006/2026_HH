# Measurements

Real numbers beat estimates in a writeup, and judges like measured numbers.
Fill these in as they are taken; note the date and the conditions.

## Software (target: week 2)

| Measurement | Target | Value | Date | Conditions |
|---|---|---|---|---|
| Time to first spoken word | < 6 s | | | |
| Vision call latency | | | | |
| TTS call latency | | | | |
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
| TTS sample rate from the real API | **UNCONFIRMED** -- 24 kHz assumed | `reference_pipeline.py --probe-rate` |

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
