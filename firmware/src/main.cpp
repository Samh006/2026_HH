// main.cpp -- Talking Reader state machine.
//
//   IDLE -> CAPTURE -> UPLOAD -> WAIT_TEXT -> UPLOAD_TTS -> SPEAKING
//     ^                                                        |
//     +------------- any button, or end of audio --------------+
//                            |
//     any failure -> SPEAK_PHRASE(error) -> IDLE
//
// Three non-negotiables from 02-SOFTWARE.md section 7:
//   1. Any button during SPEAKING stops playback immediately.
//   2. Presses during CAPTURE/UPLOAD are IGNORED, never queued -- a queued
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
    ST_IDLE = 0, ST_CAPTURE, ST_VISION, ST_SPEAKING, ST_ERROR,
};

State g_state = ST_IDLE;

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

    // The old reading stops being true the moment the user points the
    // device somewhere else. Clear it FIRST -- see repeat_clear().
    repeat_clear();

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

    // 3+4. One round trip: the server does vision AND speech, and hands back
    // audio. The device never sees the transcript, so NOTEXT and D17's
    // UNCLEAR handling both have to live server-side -- the status code is the
    // only channel it has to tell us which failure happened.
    g_state = ST_VISION;

    // Transport lives in client.cpp now; this file only decides what the user
    // hears. Note send_jpeg() buffers the WHOLE reply before returning, where
    // the old path streamed it into I2S as it arrived -- so nothing is
    // audible until the download finishes. That serialises two things that
    // used to overlap; watch the gap between these two log lines.
    int status = -1;
    std::vector<uint8_t> wav;
    const uint32_t t_send = millis();
    try {
        const std::tuple<int, std::vector<uint8_t> > reply =
            send_jpeg(SERVER_BASE_URL, jpeg, jpeg_len);
        status = std::get<0>(reply);
        wav = std::get<1>(reply);
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
    // for all of it. The old streaming path started playing partway through
    // the download instead, so this number used to be mostly hidden. It is
    // the second-largest thing on the clock after the server's own work --
    // print it every press rather than inferring it from heap lines.
    Serial.printf("[stm ] send_jpeg %lu ms (%u bytes up, %u down) -- silent "
                  "the whole time\n", (unsigned long)(millis() - t_send),
                  (unsigned)jpeg_len, (unsigned)wav.size());
    log_heap("after send_jpeg");

    if (status != 200) {
        Serial.printf("[stm ] server said %d\n", status);
        earcon_error();
        phrase_play(phrase_for_status(status));   // mapping unchanged
    } else {
        const ReadStats rs = read_aloud(wav.data(), wav.size());
        if (!rs.ok && rs.samples == 0) {
            Serial.println("[stm ] HTTP 200 but nothing played");
            earcon_error();
            phrase_play(PH_ERROR);
        }
    }
    audio_drain();
    g_state = ST_IDLE;
}

void handle_repeat() {
    // Offline by construction: the cache holds decoded PCM, so this touches
    // no network, makes no API call and takes no photo. It is the one thing
    // that still works when everything else is down.
    if (!repeat_have()) {
        // NOT PH_NO_TEXT. "Nothing found. Try moving closer." is a statement
        // about the last PHOTO; this is a statement about the device's
        // MEMORY. Saying the first when you mean the second tells a user who
        // cannot check the label that their reading failed when it did not --
        // which is exactly the bug this replaces.
        Serial.println("[stm ] repeat: nothing cached");
        earcon_error();
        phrase_play(PH_ERROR);
        g_state = ST_IDLE;
        return;
    }
    g_state = ST_SPEAKING;
    phrase_play(PH_REPEATING);
    repeat_play();
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
    Serial.printf("  server  : %s%s\n", SERVER_BASE_URL,
                  SERVER_READ_PATH);
    log_heap("boot");

    buttons_begin();
    repeat_begin();
    if (!audio_begin()) {
        Serial.println("[boot] audio failed to start -- the device cannot speak");
    }
    phrase_report_missing();

    phrase_play(PH_READY);
    Serial.println("\nready -- A short=read, A long=summarise, "
                   "B short=describe, B long=repeat\n");
}

// ── TEMPORARY DIAGNOSTIC -- 17 Sep. REVERT BEFORE SHIPPING. ──────────────
//
// The device has gone completely silent: no boot "Ready", no shutter click,
// no speech -- while the serial log insists it played everything. On 17 Sep
// the full live path worked end to end against Kristian's real server and
// reported `[play] ok 39338 samples (1.64s)` with nothing audible.
//
// git diff since the last KNOWN-GOOD audio (939135b, 11 Sep) shows audio.cpp,
// audio.h, phrase.cpp and phrase.h are byte-for-byte unchanged. So this plays
// the three phrases that DO exist in flash, one after another, with no camera,
// no Wi-Fi, no server and no capture in the way -- the exact code path that
// demonstrably made noise six days ago.
//
//   heard  -> the speaker, module, wiring and I2S are all fine, and the fault
//             is in the application path after all (which would contradict
//             the diff, and is therefore the most informative outcome)
//   silent -> the fault is below audio_write(), i.e. a signal wire, the
//             common ground, the speaker, or the module itself
//
// B short is Describe in the shipping build. Put it back.
void handle_audio_selftest() {
    Serial.println("[test] AUDIO SELF-TEST -- no camera, no wifi, no server");
    log_heap("selftest start");

    struct Item { PhraseId id; const char *name; };
    const Item items[] = {
        {PH_READY,       "ready       (0.50s)"},
        {PH_NO_TEXT,     "no_text     (1.43s)"},
        {PH_NO_INTERNET, "no_internet (1.88s)"},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        Serial.printf("[test] %u/3 playing '%s' ...\n",
                      (unsigned)(i + 1), items[i].name);
        const uint32_t t0 = millis();
        phrase_play(items[i].id);
        audio_drain();
        Serial.printf("[test]     returned after %lu ms\n",
                      (unsigned long)(millis() - t0));
        delay(400);            // a gap, so three phrases are three sounds
    }

    // Synthesised, not recorded -- so if the phrases are silent but this is
    // not, the fault is the phrase bank rather than the audio chain.
    Serial.println("[test] 4/4 shutter earcon (synthesised, 2200 Hz)");
    earcon_shutter();
    audio_drain();

    Serial.println("[test] done. Heard NOTHING at all? Then the fault is "
                   "below audio_write() -- wiring, ground, speaker or module.");
    log_heap("selftest end");
}

void loop() {
    const ButtonEvent e = buttons_poll();
    if (e != BTN_NONE) {
        Serial.printf("\n[btn ] %s\n", button_event_name(e));
        switch (e) {
            case BTN_A_SHORT: handle(MODE_READ); break;
            case BTN_A_LONG:  handle(MODE_SUMMARISE); break;
            // TEMPORARY 17 Sep: was handle(MODE_DESCRIBE). Audio
            // self-test while the device is silent. PUT IT BACK.
            case BTN_B_SHORT: handle_audio_selftest(); break;
            case BTN_B_LONG:  handle_repeat(); break;
            default: break;
        }
        // Swallow the press that stopped playback so it does not immediately
        // start a new read.
        while (buttons_any_down()) {
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
