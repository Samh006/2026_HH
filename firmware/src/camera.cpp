// camera.cpp -- capture a JPEG to PSRAM, behind the pipeline.h contract.
//
// This is the camera half of the seam described in pipeline.h. It defines
// camera_capture() and camera_release() strongly, so the weak versions in
// pipeline_stub.cpp lose at link time automatically -- no flag, no #ifdef.
//
// The actual frame grabbing lives in camera_tuning.h, which owns the
// motion-blur strategy (short exposure, high gain, best-of-N frames). This
// file owns sensor init, the pin map, and buffer lifetime.
//
// OWNERSHIP -- the one thing to get right here
//
// pipeline.h says "*jpeg points at the frame buffer (in PSRAM, owned by the
// camera driver), call camera_release() when done", which reads as
// esp_camera_fb_return(). It is NOT that. camera_grab_sharpest() grabs several
// frames and has to return each one to the driver before grabbing the next, so
// it COPIES the winner into PSRAM with heap_caps_malloc and returns that. By
// the time we hand the pointer out, the driver owns nothing.
//
// So camera_release() must free(), not esp_camera_fb_return(). Getting this
// backwards leaks a frame buffer per press (four or five presses and the
// driver runs out) or hands the driver a buffer that was never its own. The
// contract's wording is now slightly wrong, but the OBSERVABLE behaviour it
// promises -- pointer valid until camera_release(), call it exactly once -- is
// exactly what this file provides, so main.cpp needs no change.
#include "camera.h"

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>

#include "camera_tuning.h"
#include "config.h"
#include "pipeline.h"

// -- Pin map -------------------------------------------------------------
//
// Freenove ESP32-S3-WROOM CAM (D20). NOT the WROVER map in 01-HARDWARE.md
// section 2, and NOT the map in hardware/wiring.md -- both are the old board.
//
// Corroborated two ways: this is Freenove's published map for the S3-WROOM
// CAM, and CLAUDE.md independently records "GPIO 13 and 15 are camera pins on
// this board (PCLK and XCLK)", which is exactly what makes the old button
// assignment fight the camera. Still worth ten minutes against the physical
// board's pinout diagram -- hardware/wiring.md has been asking for that
// confirmation since day one and it has never been done.
//
// Overridable from config.h if a revision differs: define CAM_PIN_PWDN there
// and this whole block steps aside.
#ifndef CAM_PIN_PWDN
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD    4      // SCCB data
#define CAM_PIN_SIOC    5      // SCCB clock
#define CAM_PIN_D7     16
#define CAM_PIN_D6     17
#define CAM_PIN_D5     18
#define CAM_PIN_D4     12
#define CAM_PIN_D3     10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0     11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK   13
#endif

// 20 MHz is the standard XCLK for both the OV2640 and the OV3660 on this
// board. Drop it to 10 MHz only if frames come back striped or corrupted --
// that is a signal-integrity symptom, usually a long or unshielded ribbon.
#ifndef CAM_XCLK_HZ
#define CAM_XCLK_HZ 20000000
#endif

namespace {

bool g_inited = false;
bool g_init_attempted = false;

// The buffer we handed out, which WE own. Non-null between a successful
// camera_capture() and the matching camera_release().
uint8_t *g_frame = nullptr;

}  // namespace

bool camera_ready() { return g_inited; }

bool camera_begin() {
    if (g_init_attempted) {
        return g_inited;
    }
    g_init_attempted = true;

    // The framebuffers live in PSRAM. Without it, esp_camera_init() either
    // fails outright or silently falls back to a frame size too small to read
    // print with -- so fail loudly and early instead of shipping blurry
    // thumbnails to the model.
    if (!psramFound()) {
        Serial.println("[cam ] NO PSRAM -- refusing to init. Check "
                       "memory_type = dio_opi in platformio.ini (D21).");
        return false;
    }

    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = CAM_PIN_D0;
    cfg.pin_d1       = CAM_PIN_D1;
    cfg.pin_d2       = CAM_PIN_D2;
    cfg.pin_d3       = CAM_PIN_D3;
    cfg.pin_d4       = CAM_PIN_D4;
    cfg.pin_d5       = CAM_PIN_D5;
    cfg.pin_d6       = CAM_PIN_D6;
    cfg.pin_d7       = CAM_PIN_D7;
    cfg.pin_xclk     = CAM_PIN_XCLK;
    cfg.pin_pclk     = CAM_PIN_PCLK;
    cfg.pin_vsync    = CAM_PIN_VSYNC;
    cfg.pin_href     = CAM_PIN_HREF;
    cfg.pin_sccb_sda = CAM_PIN_SIOD;
    cfg.pin_sccb_scl = CAM_PIN_SIOC;
    cfg.pin_pwdn     = CAM_PIN_PWDN;
    cfg.pin_reset    = CAM_PIN_RESET;
    cfg.xclk_freq_hz = CAM_XCLK_HZ;

    cfg.pixel_format = PIXFORMAT_JPEG;     // hardware encoder; never raw
    cfg.frame_size   = CAM_FRAMESIZE;      // SVGA -- latency lever, config.h
    cfg.jpeg_quality = CAM_JPEG_QUALITY;   // lower number = better = bigger
    cfg.fb_count     = CAM_FB_COUNT;       // 2, so GRAB_LATEST has something
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;

    // GRAB_LATEST, not GRAB_WHEN_EMPTY. The driver header is explicit that
    // WHEN_EMPTY can hand back one of the first fb_count frames -- i.e. a
    // frame exposed while the hand was still moving on the button press. That
    // is the "sometimes sharp, sometimes blurry, no pattern" report: not a
    // focus gradient, a stale buffer. Needs fb_count >= 2 to mean anything.
    cfg.grab_mode = CAMERA_GRAB_LATEST;

    const esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[cam ] esp_camera_init failed: 0x%04x (%s)\n", err,
                      esp_err_to_name(err));
        if (err == ESP_ERR_NOT_FOUND) {
            Serial.println("[cam ] sensor not detected -- ribbon seated? "
                           "latch closed? pin map confirmed against THIS "
                           "board revision?");
        }
        return false;
    }

    g_inited = true;
    camera_tune_for_text();     // camera_tuning.h owns the blur strategy

    Serial.printf("[cam ] ready: q%d fb_count=%d psram_free=%u\n",
                  CAM_JPEG_QUALITY, CAM_FB_COUNT,
                  (unsigned)ESP.getFreePsram());
    return true;
}

// pipeline.h contract. Strong definition -- beats pipeline_stub.cpp's weak one.
bool camera_capture(const uint8_t **jpeg, size_t *len) {
    *jpeg = nullptr;
    *len = 0;

    if (!g_inited && !camera_begin()) {
        return false;
    }

    // Defensive: if a caller ever misses a camera_release(), free the previous
    // frame here rather than leaking one per press. Should never fire; if it
    // does, the log says so instead of the device quietly exhausting PSRAM
    // three minutes into a demo.
    if (g_frame != nullptr) {
        Serial.println("[cam ] WARNING previous frame never released -- "
                       "freeing it now (missing camera_release())");
        free(g_frame);
        g_frame = nullptr;
    }

    const uint32_t t0 = millis();
    uint8_t *buf = nullptr;
    size_t n = 0;
    if (!camera_grab_sharpest(&buf, &n)) {
        Serial.println("[cam ] capture failed -- no frame from the sensor");
        return false;
    }

    // A truncated JPEG is the failure D11 was written about: it uploads
    // cleanly, the server answers something plausible or nothing at all, and
    // nothing anywhere says the image was broken. Two bytes of checking here
    // turn that into one obvious serial line.
    //
    // Missing SOI means it is not a JPEG at all -- almost always the sensor
    // handing back a raw or zero-length buffer, and there is nothing useful to
    // send. Hard fail; the state machine speaks an error phrase.
    //
    // Missing EOI only means the tail is short. WARN and send it anyway: most
    // of the image is there, the model may well read it, and refusing locally
    // would turn a probably-recoverable frame into a certain failure in front
    // of the judges.
    if (n < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
        Serial.printf("[cam ] not a JPEG: %u bytes, starts %02X %02X "
                      "(want FF D8)\n", (unsigned)n,
                      n > 0 ? buf[0] : 0, n > 1 ? buf[1] : 0);
        free(buf);
        return false;
    }
    if (buf[n - 2] != 0xFF || buf[n - 1] != 0xD9) {
        Serial.printf("[cam ] WARNING no EOI marker (ends %02X %02X, want "
                      "FF D9) -- frame may be truncated, sending anyway\n",
                      buf[n - 2], buf[n - 1]);
    }

    g_frame = buf;
    *jpeg = buf;
    *len = n;

    Serial.printf("[cam ] captured %u bytes in %lu ms\n", (unsigned)n,
                  (unsigned long)(millis() - t0));
    return true;
}

// pipeline.h contract. free(), NOT esp_camera_fb_return() -- see the ownership
// note at the top of this file.
void camera_release() {
    if (g_frame == nullptr) {
        return;                 // idempotent; a double release is not a crash
    }
    free(g_frame);
    g_frame = nullptr;
}

#if USE_LOCAL_SERVER
// On the one-round-trip path main.cpp never calls vision_read() -- the server
// does vision and speech together and hands back audio. So with a real camera
// there is no stub left in the path, and the boot banner should stop saying
// there is. It is a real photo now.
//
// Deliberately NOT defined when USE_LOCAL_SERVER is 0: that path does call
// vision_read(), vision.cpp is still unwritten, and the weak stub in
// pipeline_stub.cpp correctly keeps reporting stubbed.
bool pipeline_is_stubbed() { return false; }
#endif
