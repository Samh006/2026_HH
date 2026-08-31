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
#define MOCK_BASE_URL   "http://192.168.1.100:8080/api/v1"

// Real API. HTTPS is not optional — OpenRouter serves no http:// endpoint.
// We are the client, so there is no certificate for us to obtain; we only
// skip *validating* theirs via client.setInsecure(). See 02-SOFTWARE.md §6.1
#define OR_BASE_URL     "https://openrouter.ai/api/v1"
#define OR_API_KEY      "sk-or-v1-REPLACE-ME"   // ← never commit this

// ── Models ──────────────────────────────────────────────────────────────
#define VISION_MODEL    "google/gemini-3-flash-preview"
#define TTS_MODEL       "openai/gpt-4o-mini-tts-2025-12-15"
#define TTS_VOICE       "alloy"        // must match the cloned phrase voice
#define VISION_MAX_TOK  400

// ── Audio ───────────────────────────────────────────────────────────────
// 24000 confirmed against the Messages/*.wav headers. STILL TO CONFIRM
// against a live /audio/speech response — see tools/reference_pipeline.py
#define TTS_SAMPLE_RATE 24000
#define I2S_BCK_PIN     32
#define I2S_LCK_PIN     33
#define I2S_DIN_PIN     14
#define I2S_MCLK_PIN    -1   // GPIO 0 only if the ES7148 needs MCLK (HW day 2)

// ── Buttons ─────────────────────────────────────────────────────────────
#define BTN_A_PIN       13   // Read (short) / Summarise (long)
#define BTN_B_PIN       15   // Describe (short) / Repeat (long)
#define BTN_DEBOUNCE_MS 40
#define BTN_LONGPRESS_MS 700

// ── Camera ──────────────────────────────────────────────────────────────
// Try SVGA/q12 before reaching for UXGA — biggest latency lever we have.
#define CAM_FRAMESIZE   FRAMESIZE_SVGA
#define CAM_JPEG_QUALITY 12
