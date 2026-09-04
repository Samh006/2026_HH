// pipeline_stub.cpp -- WEAK placeholders for the camera/vision half.
//
// DELETE THIS FILE once camera.cpp and vision.cpp both exist.
//
// Every definition here is __attribute__((weak)), so the moment the real
// implementations are linked in, the linker silently prefers them. That means
// the two halves of the firmware can be developed and committed independently
// without a flag, an #ifdef, or a merge conflict in main.cpp.
#include <Arduino.h>

#include "pipeline.h"

__attribute__((weak)) bool pipeline_is_stubbed() { return true; }

__attribute__((weak)) bool camera_capture(const uint8_t **jpeg, size_t *len) {
    // A real capture takes roughly this long at SVGA, so the state machine's
    // timing is not wildly optimistic while stubbed.
    delay(120);
    static const uint8_t kFakeJpeg[] = {0xFF, 0xD8, 0xFF, 0xD9};
    *jpeg = kFakeJpeg;
    *len = sizeof(kFakeJpeg);
    Serial.println("[cam ] STUB capture (4-byte fake JPEG)");
    return true;
}

__attribute__((weak)) void camera_release() {}

__attribute__((weak)) bool vision_read(const uint8_t *jpeg, size_t len,
                                       Mode mode, char *out, size_t out_sz) {
    (void)jpeg;
    (void)len;
    // Imitate the cloud round trip so the silence-filling behaviour is being
    // exercised against a realistic delay rather than an instant return.
    delay(2500);
    const char *canned;
    switch (mode) {
        case MODE_DESCRIBE:
            canned = "A white medicine bottle with a blue cap, held upright "
                     "about 20 centimetres away.";
            break;
        case MODE_SUMMARISE:
            canned = "This is a prescription label. Amoxicillin 500 milligrams, "
                     "three times a day with food.";
            break;
        default:
            canned = "Amoxicillin 500 milligrams. Take one capsule three times "
                     "a day with food. Complete the full course.";
            break;
    }
    strncpy(out, canned, out_sz - 1);
    out[out_sz - 1] = '\0';
    Serial.printf("[vis ] STUB %s -> %s\n", mode_name(mode), out);
    return true;
}
