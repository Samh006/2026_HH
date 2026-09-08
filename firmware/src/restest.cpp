// restest.cpp -- how much does frame size actually buy us?
//
//   python -m platformio run -e restest -t upload
//
// A bench experiment, not shipping firmware. Point the device at a printed
// label, press button A, and it captures the SAME scene at SVGA, UXGA and
// QXGA back to back, uploads each to the mock's /mock/upload, and prints
// bytes and milliseconds for each. Then you look at the three files in
// tools/captures/ and decide with your eyes.
//
// WHY THIS EXISTS
//
// The 8 Sep capture of a Freenove Starter Kit box settled one thing and
// raised another. "Starter Kit" came out crisp -- clean letter edges, so the
// lens is focused fine and D18's refocus is not the problem we thought. But
// the small paragraph under it was an unreadable smear, and the digits under
// the barcode were gone while the barcode's BARS resolved.
//
// That pattern is under-sampling, not blur. Those characters were about four
// pixels tall. No model and no prompt recovers four pixels; only more of them
// does. At QXGA the same glyphs land near eleven pixels, which is roughly
// where a vision model starts having a chance.
//
// So this measures the thing config.h currently asserts without evidence:
// "Try SVGA/q12 before reaching for UXGA -- biggest latency lever we have."
// That was a fair guess before anything was measured. It has now been
// measured on the other side: the whole device-side cost of a press is about
// 217 ms against a budget of roughly 5.5 s. There is room. The question is
// whether spending it buys legibility.
//
// HOW IT AVOIDS LYING TO US
//
//  * The sensor is initialised at QXGA and stepped DOWN. Framebuffers are
//    allocated at init for the configured size; growing past that mid-run
//    can fail to reallocate, and then you are quietly measuring a frame that
//    never changed size. Shrinking always fits.
//  * Each capture goes through camera_grab_sharpest() exactly as production
//    does -- CAM_DISCARD_FRAMES settling frames then best-of-CAM_SHARPEST_OF
//    -- so the milliseconds printed are the real per-press cost, not the cost
//    of one idealised frame.
//  * AE is given time to settle after each size change. Without that the
//    first size measured looks worse than it is and the ordering of the test
//    becomes the result.
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_camera.h>

#include "camera_tuning.h"
#include "config.h"

namespace {

struct Step {
    framesize_t size;
    const char *name;
    const char *dims;
};

// Ordered small to large. QXGA (2048x1536) is the OV3660's native 3 MP --
// anything above it on this sensor is interpolated and buys nothing.
const Step STEPS[] = {
    {FRAMESIZE_SVGA, "SVGA", "800x600"},      // what we ship today
    {FRAMESIZE_UXGA, "UXGA", "1600x1200"},
    {FRAMESIZE_QXGA, "QXGA", "2048x1536"},
};
constexpr int N_STEPS = sizeof(STEPS) / sizeof(STEPS[0]);

// Widest frame the sensor does. Init here so stepping down always fits.
constexpr framesize_t INIT_SIZE = FRAMESIZE_QXGA;

struct Result {
    size_t bytes;
    uint32_t ms;
    bool ok;
};
Result results[N_STEPS];

bool wifi_up() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    Serial.printf("[wifi] joining '%s'\n", WIFI_SSID_1);
    WiFi.begin(WIFI_SSID_1, WIFI_PASS_1);
    const uint32_t deadline = millis() + WIFI_TIMEOUT_MS;
    while (millis() < deadline) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[wifi] ip=%s rssi=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            return true;
        }
        delay(200);
    }
    Serial.println("[wifi] failed -- captures will not be uploaded");
    return false;
}

bool camera_up() {
    if (!psramFound()) {
        Serial.println("[cam ] no PSRAM -- cannot run this test");
        return false;
    }

    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0 = 11; cfg.pin_d1 = 9;  cfg.pin_d2 = 8;  cfg.pin_d3 = 10;
    cfg.pin_d4 = 12; cfg.pin_d5 = 18; cfg.pin_d6 = 17; cfg.pin_d7 = 16;
    cfg.pin_xclk = 15; cfg.pin_pclk = 13; cfg.pin_vsync = 6; cfg.pin_href = 7;
    cfg.pin_sccb_sda = 4; cfg.pin_sccb_scl = 5;
    cfg.pin_pwdn = -1; cfg.pin_reset = -1;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = INIT_SIZE;          // largest; we only ever step down
    cfg.jpeg_quality = CAM_JPEG_QUALITY;
    cfg.fb_count     = CAM_FB_COUNT;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;

    const esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[cam ] init failed 0x%04x (%s)\n", err,
                      esp_err_to_name(err));
        return false;
    }
    camera_tune_for_text();
    Serial.printf("[cam ] up at QXGA, psram_free=%u\n",
                  (unsigned)ESP.getFreePsram());
    return true;
}

void upload(const uint8_t *jpeg, size_t len, const char *tag) {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    char url[192];
    snprintf(url, sizeof(url), "%s/mock/upload?tag=%s", SERVER_BASE_URL, tag);
    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("       upload: http.begin failed");
        return;
    }
    http.addHeader("Content-Type", "image/jpeg");
    http.setTimeout(SERVER_TIMEOUT_MS);
    const int code = http.POST(const_cast<uint8_t *>(jpeg), len);
    Serial.printf("       upload -> HTTP %d\n", code);
    http.end();
}

void run_sweep() {
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        Serial.println("[cam ] no sensor handle");
        return;
    }

    Serial.println("\n=== resolution sweep ===");
    for (int i = 0; i < N_STEPS; i++) {
        const Step &st = STEPS[i];
        results[i] = {0, 0, false};

        if (s->set_framesize(s, st.size) != 0) {
            Serial.printf("[%-4s] set_framesize refused -- skipping\n",
                          st.name);
            continue;
        }
        // Changing frame size restarts the sensor's exposure loop. Without a
        // settle the first size measured is unfairly dark and the test order
        // becomes the finding.
        delay(400);

        uint8_t *jpeg = nullptr;
        size_t len = 0;
        const uint32_t t0 = millis();
        const bool got = camera_grab_sharpest(&jpeg, &len);
        const uint32_t ms = millis() - t0;

        if (!got) {
            Serial.printf("[%-4s] capture FAILED\n", st.name);
            continue;
        }
        results[i] = {len, ms, true};
        Serial.printf("[%-4s] %-9s %6u bytes  %4lu ms  psram_free=%u\n",
                      st.name, st.dims, (unsigned)len, (unsigned long)ms,
                      (unsigned)ESP.getFreePsram());
        upload(jpeg, len, st.name);
        free(jpeg);            // camera_grab_sharpest returns a copy we own
    }

    // The comparison table, with SVGA as the baseline since that is what
    // ships today.
    Serial.println("\n  size   dims        bytes    ms   vs SVGA");
    Serial.println("  -----  ---------  ------  ----  --------");
    for (int i = 0; i < N_STEPS; i++) {
        if (!results[i].ok) {
            Serial.printf("  %-5s  %-9s  (failed)\n", STEPS[i].name,
                          STEPS[i].dims);
            continue;
        }
        char delta[24] = "baseline";
        if (i > 0 && results[0].ok) {
            snprintf(delta, sizeof(delta), "+%lu ms",
                     (unsigned long)(results[i].ms - results[0].ms));
        }
        Serial.printf("  %-5s  %-9s  %6u  %4lu  %s\n", STEPS[i].name,
                      STEPS[i].dims, (unsigned)results[i].bytes,
                      (unsigned long)results[i].ms, delta);
    }
    Serial.println("\nLook at tools/captures/*-SVGA/-UXGA/-QXGA.jpg and "
                   "compare the SMALL print.");
    Serial.println("Press A again for another scene.\n");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== restest -- frame size sweep (bench tool) ===");
    Serial.printf("  quality q%d, fb_count %d, discard %d, best-of %d\n",
                  CAM_JPEG_QUALITY, CAM_FB_COUNT, CAM_DISCARD_FRAMES,
                  CAM_SHARPEST_OF);

    pinMode(BTN_A_PIN, INPUT_PULLUP);
    wifi_up();
    if (!camera_up()) {
        Serial.println("halted.");
        while (true) {
            delay(1000);
        }
    }
    Serial.println("\nAim at a PRINTED label, filling the frame, then press "
                   "button A.\n");
}

void loop() {
    if (digitalRead(BTN_A_PIN) == LOW) {
        delay(BTN_DEBOUNCE_MS);
        while (digitalRead(BTN_A_PIN) == LOW) {
            delay(10);
        }
        run_sweep();
    }
    delay(20);
}
