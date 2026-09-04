// config.example.h — COPY THIS TO config.h AND FILL IT IN.
//
//   cp firmware/include/config.example.h firmware/src/config.h
//
// config.h is gitignored (see .gitignore). This file is the committed template:
// if you add a setting, add it here too, or you break everyone else's build.
#pragma once

// ── Wi-Fi ───────────────────────────────────────────────────────────────
// Two networks, tried in order. Venue Wi-Fi with a captive portal will
// silently break TLS, so entries 1 and 2 should be PHONE HOTSPOTS for the
// demo — not the venue's guest network. See 02-SOFTWARE.md §11.5
#define WIFI_SSID_1     "hotspot-one"
#define WIFI_PASS_1     "password"
#define WIFI_SSID_2     "hotspot-two"
#define WIFI_PASS_2     "password"
#define WIFI_TIMEOUT_MS 15000

// ── Backend selection ───────────────────────────────────────────────────
// 1 = laptop mock over plain HTTP (weeks 1+, and the demo-day fallback)
// 0 = real OpenRouter over TLS (week 2 onward)
#define USE_MOCK_SERVER 1

// Mock: plain HTTP is deliberate. Do not add TLS here — the mock exists so
// the request/response loop is proven before TLS enters the picture.
// Set this to the laptop's LAN IP; the ESP32 cannot reach 127.0.0.1.
// PLACEHOLDER -- put YOUR laptop's LAN IP here. The mock prints it in its
// startup banner. Not 127.0.0.1: the ESP32 cannot reach that.
#define MOCK_BASE_URL   "http://192.168.1.100:8080/api/v1"

// Real API. HTTPS is not optional — OpenRouter serves no http:// endpoint.
// We are the client, so there is no certificate for us to obtain; we only
// skip *validating* theirs via client.setInsecure(). See 02-SOFTWARE.md §6.1
#define OR_BASE_URL     "https://openrouter.ai/api/v1"
#define OR_API_KEY      "sk-or-v1-REPLACE-ME"   // ← never commit this

// ── Models ──────────────────────────────────────────────────────────────
// D16 measured gemini-3.5-flash-lite at 1.4 s faster and 37% cheaper for the
// same accuracy, but says do not switch on synthetic images alone. Confirm on
// the 20-photo eval set first, then change this line.
#define VISION_MODEL    "google/gemini-3-flash-preview"
#define VISION_MAX_TOK  400

// ── Speech (D14 — read this before writing net.cpp) ─────────────────────
// There is NO /audio/speech endpoint on OpenRouter and NO response_format:
// pcm. Both return 400. Of 396 models only gpt-audio and gpt-audio-mini emit
// speech, and they do it through /chat/completions:
//
//   POST /chat/completions
//   { "model": TTS_MODEL, "stream": true,          ← audio is refused without it
//     "modalities": ["text","audio"],
//     "audio": { "voice": TTS_VOICE, "format": TTS_AUDIO_FORMAT },
//     "messages": [{ "role":"user", "content": "<text to speak>" }] }
//
// Audio comes back base64-encoded inside SSE deltas at
// choices[0].delta.audio.data — not as a raw body. So the device needs SSE
// line framing plus a base64 decoder. Each delta is independently padded, so
// it decodes standalone; keep a ONE-BYTE CARRY across deltas so a 16-bit
// sample is never split across an i2s_write(). See tools/reference_pipeline.py
// tts(), which is the working reference.
#define TTS_MODEL       "openai/gpt-audio-mini"
#define TTS_VOICE       "alloy"        // must match the phrase-bank voice
#define TTS_AUDIO_FORMAT "pcm16"

// ── Audio / pins ────────────────────────────────────────────────────────
// ESP32-S3-WROOM pin map (D20). NOT the WROVER map in 01-HARDWARE.md sect 2,
// which is now wrong for our hardware:
//   * GPIO 32/33 DO NOT EXIST on this board's header
//   * GPIO 13/15 are the camera's PCLK and XCLK -- a button there fights
//     the camera
//   * GPIO 35/36/37 are the PSRAM bus, 19/20 are native USB, 48 is the RGB
//     LED, and 0/3/45/46 are strapping pins
// Confirmed map and the reasoning: hardware/wiring.md
// ── Audio ───────────────────────────────────────────────────────────────
// 24 kHz 16-bit mono headerless — CONFIRMED against the live API (D15) and
// matching the Messages/*.wav headers. No longer an open question.
#define TTS_SAMPLE_RATE 24000
#define I2S_BCK_PIN     42
#define I2S_LCK_PIN     41
#define I2S_DIN_PIN     40
#define I2S_MCLK_PIN    -1   // -> 39 if the ES7148 needs MCLK. Any GPIO
                             // works on S3, unlike the classic ESP32

// Speaker channel -- the other half of the day-2 hardware question.
// 0 = mono, left channel only. Start here.
// 1 = duplicate every sample to both channels. Try this if the DAC is
//     visibly clocking but the speaker is silent. (02-SOFTWARE.md section 6.3)
#define I2S_DUPLICATE_TO_STEREO 0

// ── Buttons ─────────────────────────────────────────────────────────────
#define BTN_A_PIN       47   // Read (short) / Summarise (long)
#define BTN_B_PIN       21   // Describe (short) / Repeat (long)
#define BTN_DEBOUNCE_MS 40
#define BTN_LONGPRESS_MS 700

// ── Camera ──────────────────────────────────────────────────────────────
// Try SVGA/q12 before reaching for UXGA — biggest latency lever we have.
#define CAM_FRAMESIZE   FRAMESIZE_SVGA
#define CAM_JPEG_QUALITY 12
