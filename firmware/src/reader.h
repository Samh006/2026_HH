// reader.h -- one-shot path: JPEG in, speech out.
//
// For our own server (HH-2026-WebServer): POST the raw JPEG bytes to
// /api/tts/fromimage and it replies with the finished audio, having done both
// the vision call and the text-to-speech itself.
//
// This collapses what used to be two device round trips (image -> text, then
// text -> audio) into one, and it takes three things off the device:
//
//   * TLS. The server talks to OpenRouter; the device talks plain HTTP on the
//     LAN. WiFiClientSecure's 40-50 KB of internal heap -- the largest crash
//     risk in the project (02-SOFTWARE.md 6.1) -- never has to be allocated.
//   * The base64 encoder. The body is the JPEG itself, so B64Stream is not
//     needed for the upload at all.
//   * SSE framing and base64 decoding. The reply is a plain body.
//
// The device never sees the transcript on this path, which means UNCLEAR
// handling (D17) has to happen server-side -- see docs/decisions.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pipeline.h"

struct ReadStats {
    bool ok;
    uint32_t ms_to_first_audio;   // when the user actually hears something
    uint32_t ms_total;
    uint32_t samples;
    int http_status;              // <0 is a transport failure, not an HTTP code
};

// POST the JPEG and play whatever comes back, streaming it into I2S as it
// arrives rather than buffering the utterance.
//
// Deliberately tolerant of what the server sends, because the server is being
// written in parallel with this:
//   * a bare stream of 16-bit PCM  (what we want long term)
//   * a RIFF/WAVE file             (what ASP.NET's File(...,"audio/wav") emits)
//   * 32-bit float samples         (what Kokoro produces natively)
// The WAV header is parsed and skipped, and float samples are converted on the
// fly. A wrong assumption here is silent: a 44-byte header played as audio is
// a click, and float32 played as int16 is full-scale noise (see D7).
ReadStats read_aloud(const uint8_t *jpeg, size_t len, Mode mode);
