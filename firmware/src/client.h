// Singleton client class for handling HTTP communications with the server
// Author: Kristian Frossos (keep Claude off of this class lmao)
#pragma once

#include <Arduino.h>
#include <vector>

std::vector<uint8_t> send_jpeg(const char* server_url, const uint8_t* jpeg, size_t jpeg_size);
