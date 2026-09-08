// camera.h -- the camera half of pipeline.h.
//
// pipeline.h is the contract main.cpp uses and is deliberately NOT changed by
// this file: camera_capture() and camera_release() are declared there. This
// header exists only for the one thing pipeline.h has no slot for -- eager
// initialisation at boot.
//
// camera_capture() lazy-inits on first use, so main.cpp needs no change and
// works exactly as it does today. But lazy init means an unplugged ribbon is
// only discovered on the first button press, ~400 ms into a read, and reported
// as a generic error phrase. Calling camera_begin() from setup() instead moves
// that failure to the boot banner where it is obvious. Optional, recommended.
#pragma once

#include <stdbool.h>

// Initialise the sensor and apply the text-capture tuning. Safe to call more
// than once; the second and later calls are no-ops that return the first
// result. Called automatically by camera_capture() if it has not run yet.
bool camera_begin();

// True once the sensor has initialised successfully.
bool camera_ready();
