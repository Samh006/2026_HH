// pipeline.h -- the seam between the two firmware halves.
//
// The other SWE owns camera.cpp and vision.cpp and implements these two
// functions. This file is the contract; agree it once and both halves can be
// written in parallel without either waiting.
//
// pipeline_stub.cpp provides WEAK definitions of both, so main.cpp links and
// the state machine runs end to end today. When the real camera.cpp and
// vision.cpp land, their strong definitions win at link time automatically --
// no #ifdef, no flag to flip, no merge conflict. Delete the stub file once
// both are real.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum Mode : uint8_t {
    MODE_READ = 0,     // button A short -- 90% of use
    MODE_SUMMARISE,    // button A long
    MODE_DESCRIBE,     // button B short
};

// inline, and deliberately in the header rather than in pipeline_stub.cpp:
// the stub file is meant to be deleted, and anything strong-linked from there
// would break the build at exactly that moment.
inline const char *mode_name(Mode m) {
    switch (m) {
        case MODE_READ:      return "READ";
        case MODE_SUMMARISE: return "SUMMARISE";
        case MODE_DESCRIBE:  return "DESCRIBE";
        default:             return "?";
    }
}

// Capture a JPEG. On success *jpeg points at the frame buffer (in PSRAM, owned
// by the camera driver) and *len is its size. Call camera_release() when done.
bool camera_capture(const uint8_t **jpeg, size_t *len);
void camera_release();

// Upload the JPEG and get the transcription back. `out` is NUL-terminated and
// never longer than out_sz-1. Returns false on any network or API failure --
// the caller speaks an error phrase, it never fails silently.
bool vision_read(const uint8_t *jpeg, size_t len, Mode mode,
                 char *out, size_t out_sz);

// True when the stub implementations are in use, so the banner can say so and
// nobody demos a canned string thinking it came from the camera.
bool pipeline_is_stubbed();
