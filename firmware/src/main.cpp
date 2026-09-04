// main.cpp -- Talking Reader
//
// STEP 1 + 2 of the firmware build order: prove the toolchain, prove the
// buttons. No camera, no network, no audio yet -- those slot in below where
// marked, and each is testable on its own.
//
// Flash this, open the serial monitor at 115200, and press the buttons. You
// should see four distinct events and no double-fires.
//
//   pio run                        compile
//   pio run -t upload              flash
//   pio device monitor -b 115200   watch
#include <Arduino.h>

#include "buttons.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Heap logging. 02-SOFTWARE.md section 6.1: log free heap before and after
// every phase FROM DAY ONE. TLS wants 40-50 KB of internal heap and will not
// use PSRAM, so it has to coexist with the camera framebuffer -- that is where
// the crashes live. Retrofitting this logging after you have a crash costs a
// day, so it goes in before there is anything to measure.
// ---------------------------------------------------------------------------
void log_heap(const char *phase) {
    Serial.printf("[heap] %-22s free=%7u  min=%7u  largest=%7u  psram=%8u\n",
                  phase,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)ESP.getFreePsram());
}

void setup() {
    Serial.begin(115200);
    delay(300);                       // let the USB-UART settle before printing

    Serial.println();
    Serial.println("=====================================");
    Serial.println(" Talking Reader -- firmware step 1+2");
    Serial.println("=====================================");
    Serial.printf("  chip      : %s rev%d, %d core(s) @ %d MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getChipCores(), getCpuFrequencyMhz());
    Serial.printf("  flash     : %u KB\n", (unsigned)(ESP.getFlashChipSize() / 1024));
    Serial.printf("  psram     : %u KB %s\n",
                  (unsigned)(ESP.getPsramSize() / 1024),
                  ESP.getPsramSize() ? "" : "  <-- PSRAM MISSING, camera will fail");
    Serial.printf("  backend   : %s\n",
                  USE_MOCK_SERVER ? "MOCK  " MOCK_BASE_URL : "OpenRouter (TLS)");
    Serial.printf("  buttons   : A=GPIO%d  B=GPIO%d  (debounce %dms, long %dms)\n",
                  BTN_A_PIN, BTN_B_PIN, BTN_DEBOUNCE_MS, BTN_LONGPRESS_MS);
    Serial.printf("  i2s       : BCK=%d LCK=%d DIN=%d MCLK=%d  @ %d Hz\n",
                  I2S_BCK_PIN, I2S_LCK_PIN, I2S_DIN_PIN, I2S_MCLK_PIN,
                  TTS_SAMPLE_RATE);
    Serial.println();

    log_heap("boot");

    buttons_begin();
    log_heap("after buttons_begin");

    // TODO step 3  audio_begin();      I2S init + phrase playback from flash
    // TODO step 4  speech_*            SSE -> base64 -> PCM -> i2s_write
    // TODO         camera_begin();     <- other SWE
    // TODO         wifi_begin();

    Serial.println("\nready -- press a button\n");
}

void loop() {
    const ButtonEvent e = buttons_poll();
    if (e != BTN_NONE) {
        Serial.printf("[btn ] %8lu ms  %s\n",
                      (unsigned long)millis(), button_event_name(e));

        // STEP 5 will replace this with the state machine:
        //
        //   IDLE -> CAPTURE -> UPLOAD -> WAIT_TEXT -> UPLOAD_TTS -> SPEAKING
        //
        // Non-negotiables from 02-SOFTWARE.md section 7:
        //   - any button during SPEAKING stops playback immediately
        //   - presses during CAPTURE/UPLOAD are IGNORED, never queued
        //     (a queued press = a second paid call and a confusing double-read)
        //   - every failure path speaks a phrase. Silence is a bug.
    }

    // Heartbeat, so a wedged loop is obvious on the monitor.
    static uint32_t last = 0;
    if (millis() - last > 10000) {
        last = millis();
        log_heap("idle");
    }
}
