#include "client.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include "config.h"   // SERVER_TIMEOUT_MS

std::tuple<int, std::vector<uint8_t>> send_jpeg(const char* server_url, const uint8_t* jpeg, size_t jpeg_size) {
    String url = String(server_url) + "/api/tts/fromimage";
    HTTPClient http;

    if (!http.begin(url)) {
        throw std::runtime_error("Failed to initialise HTTP connection");
    }

    http.addHeader("Content-Type", "image/jpeg");

    // WITHOUT THIS THE DEVICE CANNOT TALK TO THE REAL SERVER AT ALL.
    // HTTPCLIENT_DEFAULT_TCP_TIMEOUT is 5000 ms, and the server does a vision
    // call AND Kokoro synthesis before it sends a single header byte --
    // measured 13.85 s on 17 Sep. So the default gives up at 5 s, returns a
    // transport error, and the device announces "No internet connection"
    // while connected to a working server. config.h has always had
    // SERVER_TIMEOUT_MS for this; it just was not being applied.
    http.setTimeout(SERVER_TIMEOUT_MS);

    int status = http.POST(const_cast<uint8_t*>(jpeg), jpeg_size);

    // returns early when status code indicates an error
    if (status != 200) {
        http.end();
        return {status, {}};
    }

    int response_length = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    std::vector<uint8_t> wav;

    if (response_length > 0) {
        wav.resize(response_length);
        
        // A deadline, because this loop had none. If the server sends a
        // Content-Length and then stalls -- laptop sleeps, Wi-Fi drops, Kokoro
        // dies mid-write -- available() returns 0 forever and this spins on
        // core 1. The CPU1 idle-task watchdog is not enabled in the Arduino
        // SDK config, so NOTHING on the device recovers it: the board goes
        // silent and stays silent until the battery is pulled. To a user who
        // cannot see it, that is indistinguishable from a flat battery.
        const uint32_t deadline = millis() + SERVER_TIMEOUT_MS;
        size_t bytes_read = 0;
        while (bytes_read < wav.size()) {
            if (millis() > deadline) {
                Serial.printf("[http] body stalled: %u of %u bytes\n",
                              (unsigned)bytes_read, (unsigned)wav.size());
                http.end();
                return {-2, {}};   // -2: reached the server, body incomplete
            }
            if (!stream->connected() && stream->available() == 0) {
                break;             // clean close -- play whatever arrived
            }

            size_t available = stream->available();

            if (available > 0) {
                size_t remaining = wav.size() - bytes_read;
                size_t to_read = std::min(available, remaining);
                size_t n = stream->readBytes(wav.data() + bytes_read, to_read);

                bytes_read += n;
            } else {
                delay(2);          // yield -- do not spin the core
            }
        }
    }

    http.end();

    return {status, std::move(wav)};
}
