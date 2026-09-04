// camera_tuning.h -- motion-blur mitigation for handheld text capture.
//
// Header-only and self-contained, so it drops into camera.cpp without any
// ownership or build-order argument. Two calls:
//
//   camera_tune_for_text();                    // once, after esp_camera_init
//   camera_grab_sharpest(&jpeg, &len);         // instead of esp_camera_fb_get
//
// WHY MOTION BLUR IS THE PROBLEM HERE
//
// Blur is exposure time versus hand movement. Indoors, auto-exposure happily
// picks 1/15 s to get a bright picture, and a hand holding a device 20 cm from
// a label moves further than a pixel in that time. Nothing in the cloud can
// recover it: D18 measured every candidate vision model failing near
// identically on a blurred label and succeeding on a sharp one. Sharpness is
// won on the device or not at all.
//
// AND WHY THE OV3660 MADE IT WORSE
//
// The OV3660 is 3 MP where the OV2640 was 2 MP, on a comparable die. Smaller
// pixels collect less light each, so auto-exposure needs LONGER to reach the
// same brightness. The sensor upgrade bought resolution and paid for it in
// exposure time -- which is exactly the currency motion blur is priced in.
#pragma once

// Self-contained on purpose: a header that relies on its includer having
// pulled in Arduino.h first compiles in one translation unit and fails in the
// next, which is a confusing bug to hand a teammate.
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "config.h"

// Applies text-capture settings. Call once, after esp_camera_init() succeeds.
//
// The strategy is deliberately to keep auto-exposure ON but bias it short,
// rather than pinning a manual exposure. A hardcoded exposure is sharper in
// the light you tuned it for and useless in any other, and this device gets
// pointed at a sunlit kitchen bench and a dim bedside table in the same day.
inline void camera_tune_for_text() {
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        Serial.println("[cam ] no sensor handle -- tuning skipped");
        return;
    }

    const bool is_ov3660 = (s->id.PID == OV3660_PID);
    Serial.printf("[cam ] sensor PID=0x%04x (%s)\n", s->id.PID,
                  is_ov3660 ? "OV3660" : "other");

    // Short exposure, high gain. This is the whole trick.
    s->set_gain_ctrl(s, 1);                        // auto gain on
    s->set_exposure_ctrl(s, 1);                    // auto exposure on
    s->set_aec2(s, 1);                             // the better AEC algorithm
    s->set_ae_level(s, CAM_AE_LEVEL);              // bias exposure SHORTER
    s->set_gainceiling(s, (gainceiling_t)CAM_GAINCEILING);  // let gain absorb it

    // Text is high-contrast line art, not a portrait.
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_lenc(s, 1);                             // lens shading correction
    s->set_bpc(s, 1);                              // bad pixel correct
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);

    if (is_ov3660) {
        // Only the OV3660 implements these meaningfully; on the OV2640 the
        // sharpness control is a no-op and denoise does not exist at all.
        s->set_denoise(s, CAM_DENOISE);            // clean up the extra gain
        s->set_sharpness(s, CAM_SHARPNESS);        // crisper glyph edges
        s->set_brightness(s, 1);                   // offset the -1 AE level
        // The OV3660 is commonly mounted rotated relative to the OV2640. If
        // text comes out upside down, flip these rather than rotating in the
        // cloud -- a flipped image costs the model accuracy.
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
    }

    Serial.printf("[cam ] tuned: ae_level=%d gainceiling=16x denoise=%d "
                  "sharpness=%d\n", CAM_AE_LEVEL, CAM_DENOISE, CAM_SHARPNESS);
}

// Grabs CAM_SHARPEST_OF frames and returns the sharpest, after discarding
// CAM_DISCARD_FRAMES settling frames.
//
// Sharpness is judged by JPEG size. At a fixed quality setting, a blurred
// frame contains less high-frequency detail, so the encoder emits fewer bytes.
// It is a proxy rather than a measurement -- but for several frames of the
// same scene within a second it is a reliable one, and it costs nothing:
// no decode, no Laplacian, no float maths.
//
// The winner is COPIED into PSRAM, because the driver only owns CAM_FB_COUNT
// buffers and we have to release each frame before grabbing the next.
// *** The caller must free(*jpeg) -- this is NOT a framebuffer. ***
inline bool camera_grab_sharpest(uint8_t **jpeg, size_t *len) {
    for (int i = 0; i < CAM_DISCARD_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);              // settle AEC, outlast the press
        }
    }

    uint8_t *best = nullptr;
    size_t best_len = 0;
    for (int i = 0; i < CAM_SHARPEST_OF; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == nullptr) {
            Serial.printf("[cam ] frame %d: capture failed\n", i);
            continue;
        }
        Serial.printf("[cam ] frame %d: %u bytes%s\n", i, (unsigned)fb->len,
                      fb->len > best_len ? "  <- sharpest so far" : "");
        if (fb->len > best_len) {
            uint8_t *copy = (uint8_t *)heap_caps_malloc(fb->len,
                                                        MALLOC_CAP_SPIRAM);
            if (copy == nullptr) {
                copy = (uint8_t *)malloc(fb->len);   // fall back to internal
            }
            if (copy) {
                memcpy(copy, fb->buf, fb->len);
                free(best);
                best = copy;
                best_len = fb->len;
            }
        }
        esp_camera_fb_return(fb);
    }

    if (best == nullptr) {
        return false;
    }
    Serial.printf("[cam ] kept %u bytes of %d candidates\n",
                  (unsigned)best_len, CAM_SHARPEST_OF);
    *jpeg = best;
    *len = best_len;
    return true;
}
