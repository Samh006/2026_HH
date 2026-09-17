// config.example.h — COPY THIS TO config.h AND FILL IT IN.
//
// Regenerated from a working config.h on 17 Sep. There is no API key in
// here any more and there should never be one again: the server holds it
// and the device never talks to OpenRouter directly.
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
#define WIFI_SSID_1     "your-hotspot"
#define WIFI_PASS_1     "password"
#define WIFI_SSID_2     "backup-hotspot"
#define WIFI_PASS_2     "password"
#define WIFI_TIMEOUT_MS 15000

// ── Audio ───────────────────────────────────────────────────────────────
// The rate the I2S peripheral runs at, and therefore the rate the server
// MUST send. There is no resampler on the device: the wrong rate plays at
// the wrong speed, and stereo plays as noise with no error anywhere.
// Confirmed against a live server response on 17 Sep (D15).
#define TTS_SAMPLE_RATE 24000
#define I2S_BCK_PIN     42
#define I2S_LCK_PIN     41
#define I2S_DIN_PIN     40
#define I2S_MCLK_PIN    -1   // -> 39 if the ES7148 needs MCLK. Any
                             // GPIO works on S3, unlike classic ESP32   // GPIO 0 only if the ES7148 needs MCLK (HW day 2)

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
// Measured 8 Sep on a printed box, same scene at all three sizes:
//
//   SVGA  800x600    17 KB   113 ms   barcode digits GONE, small print a smear
//   UXGA  1600x1200  62 KB   274 ms   marginal -- readable only if you guess
//   QXGA  2048x1536  99 KB   345 ms   clean: "X0012KN531", "FNK0082" both read
//
// This line used to say "try SVGA/q12 before reaching for UXGA -- biggest
// latency lever we have", which was a fair guess before anything was measured.
// It was wrong in both halves. The small print on a real label is about four
// pixels tall at SVGA, and nothing downstream recovers four pixels -- not a
// better model, not a better prompt. And the latency it was protecting turns
// out to be affordable: the whole device side of a press is ~217 ms against a
// budget of roughly 5.5 s, so +232 ms buys text that otherwise does not exist
// in the file at any price. See D28.
//
// QXGA is the OV3660's native 3 MP. Anything above it is interpolated.
#define CAM_FRAMESIZE   FRAMESIZE_QXGA
#define CAM_JPEG_QUALITY 12

// ---- Motion blur / OV3660 ----------------------------------------------
// Two frame buffers with GRAB_LATEST, not one with GRAB_WHEN_EMPTY. The
// driver header is explicit: WHEN_EMPTY means "the first fb_count frames
// might be old", so with a single buffer a press can return a frame exposed
// while the hand was still moving. That is the "sometimes clean, sometimes
// blurry" pattern -- not a gradient, a stale frame. At SVGA JPEG a second
// buffer costs tens of KB of PSRAM, of which we have 4 MB.
#define CAM_FB_COUNT    2

// Frames grabbed and thrown away after the shutter before keeping one.
// Covers AEC/AWB convergence AND the moment of hand movement on the press.
#define CAM_DISCARD_FRAMES 2

// Of this many candidates, keep the LARGEST JPEG. At fixed quality a blurrier
// frame holds less high-frequency detail and compresses smaller, so file size
// is a free sharpness proxy -- no decode, no maths. Set to 1 to disable.
#define CAM_SHARPEST_OF 3

// Bias auto-exposure SHORTER and let gain make up the light. A noisy sharp
// frame reads far better than a clean blurry one: the VLM tolerates sensor
// noise well and tolerates motion blur very badly. -2 is the shortest.
#define CAM_AE_LEVEL    -1
#define CAM_GAINCEILING GAINCEILING_16X

// OV3660 only. Its 3 MP pixels are smaller than the OV2640's 2 MP ones, so it
// needs MORE light for the same exposure -- which is why swapping sensors made
// motion blur worse, not better. Denoise cleans up the extra gain noise.
#define CAM_DENOISE     4
#define CAM_SHARPNESS   2

// ---- Offline Repeat ----------------------------------------------------
// Button B long replays the last reading from PSRAM with NO network at all --
// no server, no capture, no second API call. That is the one feature that
// still works when the laptop is asleep and the Wi-Fi is down, and it is the
// demo beat worth rehearsing: pull the network, press B long, hear it again.
//
// 30 s at 24 kHz 16-bit mono = 1.44 MB, against ~7 MB of PSRAM free after a
// QXGA capture. Measured server replies run 1.6-17.5 s, so this has real
// headroom. Allocated once at boot and never freed: a per-press allocation is
// a failure path that first shows up at press seven, in front of judges.
#define REPEAT_CACHE_SECONDS 30

// ---- The server (HH-2026-WebServer) -------------------------------------
// ONE round trip: POST the raw JPEG, get finished audio back. The server does
// the vision call AND the text-to-speech, so the device never does TLS, never
// needs a base64 encoder, never parses SSE, and -- the part that matters --
// NEVER HOLDS AN API KEY. 00-TEAM-PLAN.md section 9 named "API key sits in
// flash" as a known limitation we would have to own in the writeup; adopting
// the server removed it, and this cleanup (17 Sep) is where it actually left
// the firmware. Do not add one back.
//
// ASP.NET's default profile binds to localhost, which the ESP32 CANNOT reach.
// It must be started with:  dotnet run --urls http://0.0.0.0:5148
//
// This address is a DHCP lease. Re-check it with ipconfig on the SERVER's
// machine after any network switch -- a stale address here looks exactly like
// a dead server and costs a reflash. It has already bitten us twice.
#define SERVER_BASE_URL      "http://192.168.1.100:5148"  // <- the SERVER machine
#define SERVER_READ_PATH     "/api/tts/fromimage"
#define SERVER_SUMMARISE_PATH "/api/tts/fromimage"   // one endpoint for now
#define SERVER_DESCRIBE_PATH  "/api/tts/fromimage"

// Generous: the server does a vision call AND local Kokoro synthesis before it
// answers a single header byte, and it does not stream. Measured 17 Sep:
// 7.6 s on a real press, 13.85 s on a text-heavy image. HTTPClient's own
// default is 5 s, which is NOT enough -- client.cpp must apply this or the
// device gives up before the server answers and blames the network.
#define SERVER_TIMEOUT_MS 30000
