#include "client.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include <algorithm>
#include <stdexcept>

#include "config.h"

std::tuple<int, size_t> send_jpeg(const char* server_url, const uint8_t* jpeg,
                                  size_t jpeg_size, uint8_t* out,
                                  size_t out_cap) {
    String url = String(server_url) + SERVER_READ_PATH;
    HTTPClient http;

    if (!http.begin(url)) {
        throw std::runtime_error("Failed to initialise HTTP connection");
    }

    http.addHeader("Content-Type", "image/jpeg");
    // The server does the vision call AND synthesis before it answers a single
    // header byte, and it does not stream. Measured against the live server:
    // 2.3 s for a short label, 9.9 s for a 12-second reading. HTTPClient's own
    // default is 5 s, so without this the device gives up before the server
    // answers and then blames the network.
    http.setTimeout(SERVER_TIMEOUT_MS);

    int status = http.POST(const_cast<uint8_t*>(jpeg), jpeg_size);

    // returns early when status code indicates an error
    if (status != 200) {
        http.end();
        return {status, 0};
    }

    const int response_length = http.getSize();   // -1 when chunked
    if (out == nullptr || out_cap == 0 || response_length == 0) {
        http.end();
        return {status, 0};
    }

    size_t want = out_cap;
    if (response_length > 0) {
        if (static_cast<size_t>(response_length) > out_cap) {
            Serial.printf("[http] reply %d B exceeds the %u B cache -- "
                          "playing the first %u B only\n", response_length,
                          (unsigned)out_cap, (unsigned)out_cap);
        } else {
            want = static_cast<size_t>(response_length);
        }
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t bytes_read = 0;
    uint32_t last_progress = millis();

    while (bytes_read < want) {
        const int available = stream->available();

        if (available > 0) {
            const size_t to_read =
                std::min(static_cast<size_t>(available), want - bytes_read);
            bytes_read += stream->readBytes(out + bytes_read, to_read);
            last_progress = millis();
            continue;
        }

        // Nothing waiting. Two ways this ends badly if left unguarded: the
        // server closes early, or Content-Length over-declares. Either way
        // available() stays 0 forever, and the loop this replaces had no
        // timeout, no connected() check and no yield -- so it span at full
        // tilt, starved the idle task and tripped the task watchdog. That
        // presents as the device dying mid-press.
        if (!stream->connected() && stream->available() == 0) {
            break;                      // finished, or hung up on us
        }
        if (millis() - last_progress > SERVER_TIMEOUT_MS) {
            Serial.printf("[http] stalled after %u of %u B\n",
                          (unsigned)bytes_read, (unsigned)want);
            break;
        }
        delay(1);                       // let WiFi and the idle task run
    }

    http.end();

    return {status, bytes_read};
}
