// Singleton client class for handling HTTP communications with the server
// Author: Kristian Frossos (keep Claude off of this class lmao)
//
// Amended 18 Sep, with permission, for three things that were biting us:
//   1. The reply is written into a buffer the CALLER owns, in PSRAM. It used
//      to come back in a std::vector, which allocates in internal heap -- and
//      a real 12-second reading from this server is 585 KB against roughly
//      250 KB of free internal heap. Every long label failed with bad_alloc,
//      which main.cpp caught and reported as "no internet".
//   2. SERVER_TIMEOUT_MS is applied. HTTPClient's default is 5 s; this server
//      is measured at 2-10 s and does not answer a header byte early.
//   3. The download loop is bounded. See client.cpp.
#pragma once

#include <Arduino.h>
#include <tuple>

// Posts a jpeg to the server and streams the reply into `out`, which the
// caller owns and keeps alive (ours is the PSRAM replay cache in main.cpp, so
// a successful read is already cached for Repeat with no second copy).
//
// Returns {http status, bytes written to out}. A non-200 writes nothing --
// the status IS the error on this path, the body is not audio.
std::tuple<int, size_t> send_jpeg(const char* server_url, const uint8_t* jpeg,
                                  size_t jpeg_size, uint8_t* out,
                                  size_t out_cap);
