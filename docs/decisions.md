# Decision log

One dated line per decision. Recorded so nobody relitigates them in week 3 --
and it becomes our writeup for free.

## Carried over from the plan (2026-08-29)

| # | Date | Decision |
|---|---|---|
| D1 | 2026-08-29 | Freenove WROVER-CAM is the primary board. Kit board, built-in USB-UART so all four of us can flash it, spare GPIO. |
| D2 | 2026-08-29 | No MAX98357A needed -- the audio module is an ES7148 24-bit I2S DAC into a PAM8403. Takes digital audio directly. |
| D3 | 2026-08-29 | 4xAA NiMH into VIN, not the 9 V PP3 port. Ten times the capacity, low internal resistance, less regulator heat. |
| D4 | 2026-08-29 | Speech only, no screen. |
| D5 | 2026-08-29 | OpenRouter for vision + TTS with `response_format: pcm`. PCM goes straight into the I2S buffer, no MP3 decoder. |
| D6 | 2026-08-29 | System phrases pre-recorded to flash, voice-cloned to match the cloud TTS voice so it is one voice throughout. |

## New

| # | Date | Decision |
|---|---|---|
| D7 | 2026-08-31 | **Phrase WAVs are 16-bit signed PCM, and `tools/wav_to_phrases.py` is the only conversion path.** The three recorded WAVs in `Messages/` were 32-bit IEEE float (WAV format tag 3). Fed to `i2s_write()` configured for `I2S_BITS_PER_SAMPLE_16BIT` that plays as full-scale noise, costs double the flash, and Python's `wave` module cannot even open it -- which would also have broken the mock server. `Messages/*.wav` stays the recorded source of truth; the converter owns format, trimming, and level. |
| D8 | 2026-08-31 | **`firmware/src/phrases.h` is generated and gitignored.** 393 KB of C source for 3 phrases; regenerate with `python tools/wav_to_phrases.py --emit-header`. Committing it would make every phrase change a giant diff. |
| D9 | 2026-08-31 | **`platformio.ini` pins `espressif32@6.5.0` (Arduino-ESP32 core 2.0.14).** The I2S code in the software brief is the legacy `i2s_config_t` / `i2s_write()` driver. Core 3.x replaces it with `I2SClass`, so an unpinned platform would silently break the audio path on whoever installs next. Revisit only if we want the new API deliberately. |
| D10 | 2026-08-31 | **HTTPS: plain HTTP to the mock, TLS to OpenRouter, `setInsecure()` for cert validation.** Settles the question raised on 08-31. The mock stays plain HTTP for all four weeks -- deliberate, so that when TLS breaks we know it is TLS, and it is the demo-day fallback. But OpenRouter serves no `http://` endpoint, so TLS is not optional for the real leg. There is no certificate for us to obtain: we are the client, the cert is OpenRouter's. We skip *validating* it via `client.setInsecure()`, which is one line and removes the certificate-expiry risk on demo day. TLS lands in week 2 as an isolated step because `WiFiClientSecure` wants 40-50 KB of internal heap alongside the camera framebuffer -- it is a memory exercise, not a security checkbox, and deferring it hides that risk until week 4. Named as a known limitation in the writeup. |
| D11 | 2026-08-31 | **The mock validates uploads and can be made to fail on demand.** It base64-decodes every upload and checks SOI/EOI markers and padding, because a streaming base64 encoder that drops its tail is the likeliest firmware bug and is invisible from the device end. `POST /mock/scenario` switches between `ok, notext, http500, timeout, garbage, empty` so the state machine's failure paths are testable without unplugging anything. |
| D12 | 2026-08-31 | **The eval scorer treats number words and digits as the same fact.** "three times a day" and "3 times a day" are the same dose and the model correctly transcribes whichever is printed; without normalising, every run drowns in false failures. Month names normalise too, so "12 September" and "12/09" score alike. |
| D13 | 2026-08-31 | **`make_phrases.py` must not run until the cloud TTS voice is chosen.** The point of D6 is cloning the voice we actually ship, so generating the remaining 6 phrases first means doing it twice. Ordering: pick voice -> capture a sample with `reference_pipeline.py` -> clone -> generate. |

## Open questions

| Question | Owner | Needed by |
|---|---|---|
| Does the ES7148 need MCLK on SCK? (try unconnected, then grounded, then GPIO 0) | Hardware | Day 2 -- blocks `audio.cpp` |
| Which channel is the speaker wired to? `ONLY_LEFT` vs `RIGHT_LEFT` | Hardware | Day 2 |
| Confirmed camera pin list for this exact board revision | Hardware | Day 1 |
| Real TTS sample rate from a live `/audio/speech` response (24 kHz assumed, matches the recorded WAVs but unconfirmed against the API) | Cloud | Day 1 -- `reference_pipeline.py --probe-rate` |
| Which cloud TTS voice do we ship? Blocks D13. | Cloud | Before phrase generation |
| Laptop LAN IP for `MOCK_BASE_URL` in each dev's `config.h` | Everyone | Day 2 |
