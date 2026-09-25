// main.cpp -- Talking Reader state machine.
//
//   IDLE -> CAPTURE -> SEND -> SPEAKING -> IDLE
//     ^                                      |
//     +--------- any button, or end of audio +
//                            |
//     any failure -> SPEAK_PHRASE(error) -> IDLE
//
// ONE round trip, one backend: the server does vision AND speech and hands
// back finished audio (D25, D29). The device never sees the transcript, never
// does TLS, never holds an API key. The mock and OpenRouter paths are gone --
// there is no #if here to pick a backend any more, because there is only one.
//
// Three non-negotiables from 02-SOFTWARE.md section 7:
//   1. Any button during SPEAKING stops playback immediately.
//   2. Presses during CAPTURE/SEND are IGNORED, never queued -- a queued
//      press is a second paid API call and a confusing double-read.
//   3. Every failure path speaks. Silence is a bug.
//
//   pio run -t upload && pio device monitor -b 115200
#include <Arduino.h>
#include <WiFi.h>

#include "audio.h"
#include "buttons.h"
#include "client.h"
#include "config.h"
#include "phrase.h"
#include "pipeline.h"
#include "reader.h"

namespace {

enum State : uint8_t {
    ST_IDLE = 0, ST_CAPTURE, ST_SEND, ST_SPEAKING,
};

State g_state = ST_IDLE;

// ---- Offline Repeat ------------------------------------------------------
// The reply from the server is downloaded straight into this buffer, so a
// successful read is already cached: Repeat costs no network, no capture and
// no second API call, and it is the one feature that still works when the
// laptop is asleep. Allocated once at boot and never freed -- a per-press
// allocation of 1.4 MB is a failure path that first shows up at press seven,
// in front of judges. See config.h REPEAT_CACHE_SECONDS.
//
// It has to be PSRAM. A real 12-second reading from this server measured
// 585 KB, against roughly 250 KB of free internal heap.
constexpr size_t REPEAT_CAP =
    (size_t)REPEAT_CACHE_SECONDS * TTS_SAMPLE_RATE * 2 + 64;
uint8_t *g_repeat = nullptr;      // nullptr if the allocation failed
size_t g_repeat_len = 0;          // 0 = nothing cached yet

void log_heap(const char *phase) {
    Serial.printf("[heap] %-22s free=%7u  min=%7u  largest=%7u  psram=%8u\n",
                  phase, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)ESP.getFreePsram());
}

bool wifi_connect() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    phrase_play(PH_CONNECTING);
    const char *ssids[] = {WIFI_SSID_1, WIFI_SSID_2};
    const char *passes[] = {WIFI_PASS_1, WIFI_PASS_2};
    for (int i = 0; i < 2; i++) {
        Serial.printf("[wifi] trying '%s'\n", ssids[i]);
        WiFi.begin(ssids[i], passes[i]);
        const uint32_t deadline = millis() + WIFI_TIMEOUT_MS;
        while (millis() < deadline) {
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[wifi] connected, ip=%s rssi=%d\n",
                              WiFi.localIP().toString().c_str(), WiFi.RSSI());
                return true;
            }
            delay(200);
        }
        WiFi.disconnect();
    }
    Serial.println("[wifi] both networks failed");
    return false;
}

#if WIFI_SCAN_AT_BOOT
const char *auth_name(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa-psk";
        case WIFI_AUTH_WPA2_PSK:        return "wpa2-psk";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/wpa2-psk";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3-psk";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/wpa3-psk";
        default:                        return "?";
    }
}

// The S3 radio is 2.4 GHz ONLY. A 5 GHz-only SSID is not a firmware problem
// and no amount of retrying will fix it, so print what the board itself can
// see rather than inferring it from a laptop that has both bands. The same
// list also settles the Curtin question (CLAUDE.md): our code does pre-shared
// keys only, so an enterprise network is equally unjoinable.
void wifi_scan_report() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    const int n = WiFi.scanNetworks();
    Serial.printf("[scan] %d network(s) visible to the 2.4 GHz radio\n", n);

    bool saw_1 = false;
    bool saw_2 = false;
    for (int i = 0; i < n; i++) {
        const String ssid = WiFi.SSID(i);
        const bool is_1 = (ssid == WIFI_SSID_1);
        const bool is_2 = (ssid == WIFI_SSID_2);
        saw_1 = saw_1 || is_1;
        saw_2 = saw_2 || is_2;
        Serial.printf("  %-24s ch%-3d %4d dBm  %-15s%s\n", ssid.c_str(),
                      WiFi.channel(i), WiFi.RSSI(i),
                      auth_name(WiFi.encryptionType(i)),
                      is_1 ? " <-- WIFI_SSID_1"
                           : is_2 ? " <-- WIFI_SSID_2" : "");
    }
    WiFi.scanDelete();

    if (!saw_1 && !saw_2) {
        Serial.printf("[scan] NEITHER %s nor %s is on 2.4 GHz here. The board "
                      "cannot join either one -- move the board AND the server "
                      "to a 2.4 GHz network.\n", WIFI_SSID_1, WIFI_SSID_2);
    } else if (!saw_1) {
        Serial.printf("[scan] %s is NOT on 2.4 GHz here; the fallback %s is. "
                      "The server must be on that network too.\n",
                      WIFI_SSID_1, WIFI_SSID_2);
    }
}
#endif  // WIFI_SCAN_AT_BOOT

// Our server's HTTP status is the only thing the device learns about a
// failure on the one-round-trip path, so the mapping IS the error handling.
// Negative values are HTTPClient transport errors, not server responses.
PhraseId phrase_for_status(int status) {
    if (status <= 0) {
        return PH_NO_INTERNET;      // never reached the server at all
    }
    switch (status) {
        case 422: return PH_NO_TEXT;      // read fine, nothing legible in it
        case 502:
        case 503: return PH_NO_INTERNET;  // server could not reach OpenRouter
        default:  return PH_ERROR;
    }
    // NOTE: uncertainty (D17) is deliberately NOT a status code here. A
    // non-200 makes this path play a phrase and skip the audio entirely --
    // so returning 409 for "the numbers might be wrong" would suppress the
    // very reading the user asked for. The server folds the warning into the
    // speech it generates instead, which also works today while PH_UNCERTAIN
    // is still unrecorded. If we later want it in the recorded voice, add a
    // response header and play the phrase before streaming the 200 body.
}

void handle(Mode mode) {
    log_heap("press");

    // 1. Immediate physical confirmation, before anything slow.
    earcon_shutter();
    phrase_play(mode == MODE_DESCRIBE ? PH_DESCRIBING : PH_READING);

    if (!wifi_connect()) {
        earcon_error();
        phrase_play(PH_NO_INTERNET);
        g_state = ST_IDLE;
        return;
    }

    // 2. Capture.
    g_state = ST_CAPTURE;
    const uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    if (!camera_capture(&jpeg, &jpeg_len)) {
        Serial.println("[stm ] capture failed");
        earcon_error();
        phrase_play(PH_ERROR);
        g_state = ST_IDLE;
        return;
    }
    Serial.printf("[stm ] captured %u bytes\n", (unsigned)jpeg_len);
    log_heap("after capture");

    // 3. One round trip: the server does vision AND speech and hands back
    // audio. The device never sees the transcript, so NOTEXT and D17's UNCLEAR
    // handling both live server-side -- the status code is the only channel it
    // has to tell us which failure happened.
    //
    // All three modes POST the same path today: the server cannot tell them
    // apart (SERVER_CONTRACT.md, "One endpoint only"), so A-long and B-short
    // deliberately behave as a plain read rather than pretending otherwise.
    g_state = ST_SEND;

    int status = -1;
    size_t wav_len = 0;
    const uint32_t t_send = millis();
    try {
        const std::tuple<int, size_t> reply =
            send_jpeg(SERVER_BASE_URL, jpeg, jpeg_len, g_repeat, REPEAT_CAP);
        status = std::get<0>(reply);
        wav_len = std::get<1>(reply);
    } catch (const std::exception &e) {
        // send_jpeg throws if http.begin() fails. Uncaught, that is
        // std::terminate -> abort -> reboot, which on demo day looks like the
        // device dying in someone's hand. A transport failure has to sound
        // like every other transport failure instead.
        Serial.printf("[stm ] send_jpeg threw: %s\n", e.what());
        status = -1;
    }
    camera_release();
    // This is the whole cost of the buffered design: the user hears nothing
    // for all of it, because the server does not answer a header byte until
    // vision and synthesis are both done. Measured against the live server:
    // 2.3 s for a short label, 9.9 s for a 12-second reading. It is the
    // largest thing on the clock, so print it every press.
    Serial.printf("[stm ] send_jpeg %lu ms (%u bytes up, %u down) -- silent "
                  "the whole time\n", (unsigned long)(millis() - t_send),
                  (unsigned)jpeg_len, (unsigned)wav_len);
    log_heap("after send_jpeg");

    if (status != 200) {
        // Nothing was written to the cache on a non-200, so whatever Repeat
        // was holding is still good and still the last thing the user heard.
        Serial.printf("[stm ] server said %d\n", status);
        earcon_error();
        phrase_play(phrase_for_status(status));
    } else if (wav_len == 0) {
        Serial.println("[stm ] HTTP 200 with an empty body");
        g_repeat_len = 0;
        earcon_error();
        phrase_play(PH_ERROR);
    } else {
        // The audio is already sitting in the replay cache -- it was
        // downloaded straight into it, so Repeat needs no copy.
        g_repeat_len = wav_len;
        g_state = ST_SPEAKING;
        const ReadStats rs = read_aloud(g_repeat, g_repeat_len);
        if (!rs.ok && rs.samples == 0) {
            Serial.println("[stm ] HTTP 200 but nothing played");
            earcon_error();
            phrase_play(PH_ERROR);
        }
    }
    audio_drain();
    g_state = ST_IDLE;
}

// Button B long. No server, no capture, no network at all -- replay the bytes
// the server sent last time, straight out of PSRAM. Pull the Wi-Fi and this
// still works, which is the whole point of it.
void handle_repeat() {
    if (g_repeat == nullptr || g_repeat_len == 0) {
        Serial.println("[stm ] repeat with nothing cached");
        phrase_play(PH_NO_TEXT);
        g_state = ST_IDLE;
        return;
    }
    Serial.printf("[stm ] repeat %u bytes from PSRAM\n", (unsigned)g_repeat_len);
    phrase_play(PH_REPEATING);
    g_state = ST_SPEAKING;
    read_aloud(g_repeat, g_repeat_len);
    audio_drain();
    g_state = ST_IDLE;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println("\n=====================================");
    Serial.println(" Talking Reader");
    Serial.println("=====================================");
    Serial.printf("  chip    : %s rev%d @ %d MHz\n", ESP.getChipModel(),
                  ESP.getChipRevision(), getCpuFrequencyMhz());
    Serial.printf("  psram   : %u KB%s\n", (unsigned)(ESP.getPsramSize() / 1024),
                  ESP.getPsramSize() ? "" : "   <-- MISSING, camera will fail");
    Serial.printf("  server  : %s%s\n", SERVER_BASE_URL, SERVER_READ_PATH);
    if (pipeline_is_stubbed()) {
        Serial.println("  pipeline: *** STUBBED *** camera/vision are fake -- "
                       "text below is canned, not read from a photo");
    }
    log_heap("boot");

    g_repeat = (uint8_t *)ps_malloc(REPEAT_CAP);
    if (g_repeat == nullptr) {
        // This is the buffer the server's reply is downloaded INTO, so losing
        // it loses reading as well as Repeat -- every press would report an
        // empty body. Say so plainly rather than crashing on press one. It has
        // never happened: 1.4 MB out of 8 MB, claimed at boot before the
        // camera takes its framebuffers.
        Serial.printf("[boot] repeat cache: ps_malloc(%u) FAILED -- READING "
                      "AND REPEAT ARE BOTH DEAD until this is fixed\n",
                      (unsigned)REPEAT_CAP);
    } else {
        Serial.printf("[boot] repeat cache: %u B in PSRAM (%d s ceiling)\n",
                      (unsigned)REPEAT_CAP, (int)REPEAT_CACHE_SECONDS);
    }

    buttons_begin();
    if (!audio_begin()) {
        Serial.println("[boot] audio failed to start -- the device cannot speak");
    }
    phrase_report_missing();

#if WIFI_SCAN_AT_BOOT
    wifi_scan_report();
#endif

    phrase_play(PH_READY);
    Serial.println("\nready -- A = read a label, B = say it again\n");
}

void loop() {
    const ButtonEvent e = buttons_poll();
    if (e != BTN_NONE) {
        Serial.printf("\n[btn ] %s\n", button_event_name(e));
        switch (e) {
            case BTN_A_SHORT:
                handle(MODE_READ);
                break;

            // Deliberately unassigned. A long used to run MODE_SUMMARISE and
            // B short MODE_DESCRIBE, but all three modes POST the same
            // endpoint and the server cannot tell them apart
            // (SERVER_CONTRACT.md, "One endpoint only") -- so three of the
            // four gestures did exactly the same thing, each costing an API
            // call and a ~17 s wait. An accidental long hold is easy for
            // someone with a tremor, and our users are the least able to
            // notice they made one.
            case BTN_A_LONG:
                Serial.println("[btn ] A long is unassigned -- nothing to do");
                break;

            // B replays, however it is pressed. Nobody holding a button for a
            // second should have to discover it means something else.
            case BTN_B_SHORT:
            case BTN_B_LONG:
                handle_repeat();
                break;

            default: break;
        }
        // Swallow the press that stopped playback so it does not immediately
        // start a new read. Bounded: a pin stuck LOW -- miswired, shorted, or
        // a jammed cap -- used to spin here forever, and because audio.cpp
        // also polls buttons_any_down() inside every playback chunk it
        // silenced the device at the same time. That combination is
        // indistinguishable from a dead board. Now it is a log line.
        const uint32_t swallow_deadline = millis() + 3000;
        while (buttons_any_down()) {
            if ((int32_t)(millis() - swallow_deadline) > 0) {
                Serial.printf("[btn ] a button has been down for 3 s -- check "
                              "GPIO %d and GPIO %d for a stuck pin\n",
                              (int)BTN_A_PIN, (int)BTN_B_PIN);
                break;
            }
            delay(10);
        }
        audio_clear_stop();
        buttons_poll();
    }

    static uint32_t last = 0;
    if (g_state == ST_IDLE && millis() - last > 30000) {
        last = millis();
        log_heap("idle");
    }
}
