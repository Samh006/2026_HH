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
#include "speech.h"

namespace {

enum State : uint8_t {
    ST_IDLE = 0, ST_CAPTURE, ST_VISION, ST_SPEAKING, ST_ERROR,
};

State g_state = ST_IDLE;

// Last result, for Repeat. Lives in PSRAM-free internal RAM: 1 KB is cheap and
// Repeat must work with no network at all.
constexpr size_t TEXT_MAX = 1024;
char g_last_text[TEXT_MAX] = {0};
bool g_have_last = false;

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

// D17: models confidently invent digits on a hard image rather than abstain.
// The prompt now asks them to mark what they cannot read, and the device has
// to actually voice that doubt -- a silent [?] is worse than useless to
// someone who cannot check the label themselves.
bool text_is_uncertain(const char *s) {
    return strstr(s, "UNCLEAR") != nullptr || strstr(s, "[?]") != nullptr;
}

void speak_result(const char *text) {
    g_state = ST_SPEAKING;

    if (text_is_uncertain(text)) {
        Serial.println("[stm ] uncertainty markers present -- warning first");
        phrase_play(PH_UNCERTAIN);
    }

    const SpeechStats st = speech_say(text);
    if (!st.ok && st.samples == 0) {
        // Nothing came out at all. That is a failure path, so it speaks.
        Serial.println("[stm ] TTS produced no audio");
        earcon_error();
        phrase_play(PH_ERROR);
    }
    audio_drain();
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

#if USE_LOCAL_SERVER
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
    return;
#else
    // 3. Vision.
    g_state = ST_VISION;
    char text[TEXT_MAX];
    const bool got = vision_read(jpeg, jpeg_len, mode, text, sizeof(text));
    camera_release();
    log_heap("after vision");

    if (!got) {
        Serial.println("[stm ] vision failed");
        earcon_error();
        phrase_play(PH_ERROR);
        g_state = ST_IDLE;
        return;
    }

    // NOTEXT is handled here, on the device, with a recorded phrase. Sending
    // it to TTS would cost money and sound like a malfunction.
    if (strcmp(text, "NOTEXT") == 0) {
        Serial.println("[stm ] NOTEXT");
        phrase_play(PH_NO_TEXT);
        g_state = ST_IDLE;
        return;
    }

    strncpy(g_last_text, text, TEXT_MAX - 1);
    g_last_text[TEXT_MAX - 1] = '\0';
    g_have_last = true;

    // 4. Speak.
    speak_result(text);
    log_heap("after speech");
    g_state = ST_IDLE;
#endif  // USE_LOCAL_SERVER
}

void handle_repeat() {
    if (!g_have_last) {
        Serial.println("[stm ] repeat with nothing cached");
        phrase_play(PH_NO_TEXT);
        return;
    }
    phrase_play(PH_REPEATING);
    // Note: this still calls TTS. Caching the PCM rather than the text would
    // make Repeat work fully offline, which the plan wants -- but 20 s of
    // audio is ~1 MB, so it has to live in PSRAM. Worth doing once the happy
    // path is proven.
    speak_result(g_last_text);
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
    Serial.printf("  backend : %s\n",
                  USE_MOCK_SERVER ? "MOCK " MOCK_BASE_URL : "OpenRouter (TLS)");
    if (pipeline_is_stubbed()) {
        Serial.println("  pipeline: *** STUBBED *** camera/vision are fake -- "
                       "text below is canned, not read from a photo");
    }
    log_heap("boot");

    buttons_begin();
    if (!audio_begin()) {
        Serial.println("[boot] audio failed to start -- the device cannot speak");
    }
    phrase_report_missing();

    phrase_play(PH_READY);
    Serial.println("\nready -- A short=read, A long=summarise, "
                   "B short=describe, B long=repeat\n");
}

void loop() {
    const ButtonEvent e = buttons_poll();
    if (e != BTN_NONE) {
        Serial.printf("\n[btn ] %s\n", button_event_name(e));
        switch (e) {
            case BTN_A_SHORT: handle(MODE_READ); break;
            case BTN_A_LONG:  handle(MODE_SUMMARISE); break;
            case BTN_B_SHORT: handle(MODE_DESCRIBE); break;
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
