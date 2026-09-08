#include "client.h"

#include <HTTPClient.h>
#include <WiFi.h>

std::vector<uint8_t> send_jpeg(const char* server_url, const uint8_t* jpeg, size_t jpeg_size) {
    String url = String(server_url) + "/api/tts/fromimage";
    HTTPClient http;

    if (!http.begin(url)) {
        throw std::runtime_error("Failed to initialise HTTP connection");
    }

    http.addHeader("Content-Type", "image/jpeg");

    int status = http.POST(const_cast<uint8_t*>(jpeg), jpeg_size);

    // Current catch-all error handling, swap this out for more specific error handling
    if (status != 200) {
        http.end();
        throw std::runtime_error("Server returned an error");
    }

    int response_length = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    std::vector<uint8_t> wav;

    if (response_length > 0) {
        wav.resize(response_length);
        
        size_t bytes_read = 0;
        while (bytes_read < wav.size()) {
            size_t available = stream->available();

            if (available > 0) {
                size_t remaining = wav.size() - bytes_read;
                size_t to_read = std::min(available, remaining);
                size_t n = stream->readBytes(wav.data() + bytes_read, to_read);

                bytes_read += n;
            }
        }
    }

    http.end();

    return wav;
}
