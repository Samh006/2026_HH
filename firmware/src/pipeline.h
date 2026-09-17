// pipeline.h -- the camera seam.
//
// This was the contract between the two firmware halves while camera.cpp and
// vision.cpp were being written in parallel, with pipeline_stub.cpp providing
// weak definitions so main.cpp could link before either existed.
//
// Both of those are gone now (17 Sep). camera.cpp is real and confirmed on
// silicon; vision.cpp was never needed, because the server does the vision
// call and returns finished audio in one round trip. So all that survives is
// the camera's two functions and the Mode enum.
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

