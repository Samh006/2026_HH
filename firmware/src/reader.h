// reader.h -- play a buffer of audio out of the speaker.
//
// The HTTP half of this file moved to client.h (send_jpeg). What is left is
// the part that was always the fiddly bit: working out what the server
// actually sent and getting it into I2S without mangling it.
//
// Deliberately tolerant of the format, because the server is still moving:
//   * a bare stream of 16-bit PCM  (what we want long term)
//   * a RIFF/WAVE file             (what ASP.NET's File(...,"audio/wav") emits)
//   * 32-bit float samples         (what Kokoro produces natively)
// A wrong assumption here is silent: a 44-byte header played as audio is a
// click, and float32 played as int16 is full-scale noise (D7).
#pragma once

#include <stddef.h>
#include <stdint.h>

struct ReadStats {
    bool ok;
    uint32_t ms_to_first_audio;   // from entering this function, NOT from the
                                  // button press -- see the note below
    uint32_t ms_total;
    uint32_t samples;
};

// Play `len` bytes of audio. Accepts a WAV or bare PCM; returns once the
// audio has been queued, or early if a button press stopped it.
//
// NOTE ON ms_to_first_audio. This function used to stream from the socket, so
// the field measured the real thing: how long until the user heard a sound.
// The caller now downloads the whole body before calling us, so it only
// measures how fast we start playing a buffer we were handed -- a few
// milliseconds, always. What the user actually feels is
//   capture + send_jpeg + this
// and only the caller can add those up. Do not quote this number as latency.
ReadStats read_aloud(const uint8_t *body, size_t len);
