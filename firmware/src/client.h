// Singleton client class for handling HTTP communications with the server
// Author: Kristian Frossos (keep Claude off of this class lmao)
#pragma once

#include <Arduino.h>
#include <vector>
#include <tuple>

// Posts a jpeg to the server and gets a response, returning the status code and response content in a tuple.
std::tuple<int, std::vector<uint8_t>> send_jpeg(const char* server_url, const uint8_t* jpeg, size_t jpeg_size);
