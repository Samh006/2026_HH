// smoke.cpp -- is the chip running the app at all?
//
// Built only by [env:smoke]. The real firmware excludes it.
//   python -m platformio run -e smoke -t upload
//
// WHY THIS EXISTS: nothing prints on COM3 -- not the full firmware, not an
// 8-line sketch. But the S3's USB-Serial-JTAG peripheral stays enumerated
// whether or not the app boots, so a live COM port proves nothing. This gives
// three liveness signals that do NOT depend on serial:
//
//   1. RGB LED on GPIO 48 cycles red -> green -> blue, one second each.
//   2. GPIO 2 toggles every second (put a multimeter on it: 0 V / 3.3 V).
//   3. Serial prints, for whichever port turns out to carry it.
//
// If you SEE the LED cycle, the app is running and the fault is serial-only.
// If you see nothing on any of the three, the image is not booting.
#include <Arduino.h>

#define RGB_PIN     48    // Freenove ESP32-S3 onboard WS2812
#define TOGGLE_PIN  2     // free pin, safe to probe

void setup() {
    Serial.begin(115200);
    pinMode(TOGGLE_PIN, OUTPUT);
}

void loop() {
    static uint32_t n = 0;
    const uint8_t phase = n % 3;

    neopixelWrite(RGB_PIN,
                  phase == 0 ? 60 : 0,
                  phase == 1 ? 60 : 0,
                  phase == 2 ? 60 : 0);
    digitalWrite(TOGGLE_PIN, n & 1);

    Serial.printf("alive n=%lu t=%lums rgb=%s psram=%u heap=%u chip=%s\n",
                  (unsigned long)n, (unsigned long)millis(),
                  phase == 0 ? "R" : (phase == 1 ? "G" : "B"),
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap(),
                  ESP.getChipModel());
    n++;
    delay(1000);
}
