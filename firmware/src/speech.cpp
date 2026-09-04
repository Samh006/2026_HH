#include "speech.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "audio.h"
#include "config.h"

namespace {

// SSE lines carrying audio are large: the real API sent 13 chunks totalling
// 232 KB, so a single line's base64 runs to tens of kilobytes. That buffer
// lives in PSRAM, not internal heap -- internal heap is the scarce resource
// TLS needs 40-50 KB of, and putting a 48 KB line buffer next to it is how you
// get a handshake failure that looks like a network bug.
constexpr size_t LINE_CAP = 48 * 1024;
char *g_line = nullptr;

// Decoded PCM is pushed to I2S in small batches. Kept internal and small --
// this is a working buffer, not a store.
constexpr size_t PCM_BATCH = 512;

const int8_t B64_REV[256] = {
    // -1 invalid, -2 padding '='
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

// Decodes base64 straight into I2S. The one piece of state that MUST survive
// across deltas is `odd` -- the leftover byte when a decode ends on an odd
// count. A 16-bit sample split across two i2s_write() calls is a periodic
// click, and it is an evening to find. (02-SOFTWARE.md section 11.4)
struct PcmSink {
    int16_t batch[PCM_BATCH];
    size_t n = 0;
    uint8_t odd = 0;
    bool has_odd = false;
    uint32_t samples = 0;
    bool aborted = false;

    bool push_byte(uint8_t b) {
        if (!has_odd) {
            odd = b;
            has_odd = true;
            return true;
        }
        // little-endian: first byte is the low half
        batch[n++] = (int16_t)((uint16_t)odd | ((uint16_t)b << 8));
        has_odd = false;
        samples++;
        if (n == PCM_BATCH) {
            return flush();
        }
        return true;
    }

    bool flush() {
        if (n == 0) {
            return true;
        }
        const bool ok = audio_write(batch, n);
        n = 0;
        if (!ok) {
            aborted = true;
        }
        return ok;
    }
};

// Decode one base64 run (up to the closing quote) into the sink.
bool decode_b64_run(const char *p, const char *end, PcmSink &sink) {
    uint32_t acc = 0;
    int acc_bits = 0;
    for (; p < end; p++) {
        const int8_t v = B64_REV[(uint8_t)*p];
        if (v == -2) {
            break;                       // '=' padding: run is over
        }
        if (v < 0) {
            continue;                    // whitespace or stray char
        }
        acc = (acc << 6) | (uint8_t)v;
        acc_bits += 6;
        if (acc_bits >= 8) {
            acc_bits -= 8;
            if (!sink.push_byte((uint8_t)((acc >> acc_bits) & 0xFF))) {
                return false;
            }
        }
    }
    return true;
}

// Read one line (up to '\n') from the stream into g_line. Returns length, or
// -1 on timeout/disconnect. Trailing '\r' is stripped.
int read_line(WiFiClient *s, uint32_t timeout_ms) {
    size_t len = 0;
    const uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        if (!s->connected() && s->available() == 0) {
            return len > 0 ? (int)len : -1;
        }
        const int c = s->read();
        if (c < 0) {
            delay(1);
            continue;
        }
        if (c == '\n') {
            while (len > 0 && g_line[len - 1] == '\r') {
                len--;
            }
            g_line[len] = '\0';
            return (int)len;
        }
        if (len < LINE_CAP - 1) {
            g_line[len++] = (char)c;
        }
        // Overlong lines are truncated rather than grown: a runaway line means
        // something is wrong upstream, and growing into PSRAM forever hides it.
    }
    return -1;
}

}  // namespace

SpeechStats speech_say(const char *text) {
    SpeechStats st = {};
    st.ok = false;
    st.http_status = 0;

    if (text == nullptr || *text == '\0') {
        Serial.println("[tts ] refusing to speak empty text");
        return st;
    }

    if (g_line == nullptr) {
        g_line = (char *)ps_malloc(LINE_CAP);
        if (g_line == nullptr) {
            Serial.println("[tts ] ps_malloc for the SSE line buffer failed");
            return st;
        }
    }

    // Request body. Built with ArduinoJson rather than snprintf because the
    // text is transcribed from a photo and will contain quotes, apostrophes
    // and newlines that must be escaped correctly.
    DynamicJsonDocument doc(2048);
    doc["model"] = TTS_MODEL;
    doc["stream"] = true;                     // audio is refused without this
    JsonArray mod = doc.createNestedArray("modalities");
    mod.add("text");
    mod.add("audio");
    JsonObject audio = doc.createNestedObject("audio");
    audio["voice"] = TTS_VOICE;
    audio["format"] = TTS_AUDIO_FORMAT;
    JsonArray msgs = doc.createNestedArray("messages");
    JsonObject m = msgs.createNestedObject();
    m["role"] = "user";
    m["content"] = text;

    String body;
    serializeJson(doc, body);
    doc.clear();

    const uint32_t t0 = millis();

#if USE_MOCK_SERVER
    WiFiClient client;
    const String url = String(MOCK_BASE_URL) + "/chat/completions";
#else
    WiFiClientSecure client;
    // Hackathon-acceptable, and it removes certificate-expiry risk on demo
    // day. Named as a known limitation in the writeup rather than hidden.
    client.setInsecure();
    const String url = String(OR_BASE_URL) + "/chat/completions";
#endif

    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("[tts ] http.begin failed");
        return st;
    }
    http.addHeader("Content-Type", "application/json");
#if !USE_MOCK_SERVER
    http.addHeader("Authorization", "Bearer " OR_API_KEY);
#endif
    http.setTimeout(20000);

    Serial.printf("[tts ] POST %s (%u byte body)\n", url.c_str(), body.length());
    st.http_status = http.POST(body);
    if (st.http_status != HTTP_CODE_OK) {
        Serial.printf("[tts ] HTTP %d: %s\n", st.http_status,
                      http.getString().substring(0, 160).c_str());
        http.end();
        return st;
    }

    WiFiClient *stream = http.getStreamPtr();
    PcmSink sink;
    bool saw_done = false;
    uint32_t t_first = 0;

    while (true) {
        const int len = read_line(stream, 15000);
        if (len < 0) {
            Serial.println("[tts ] stream ended without [DONE]");
            break;
        }
        if (len == 0 || strncmp(g_line, "data:", 5) != 0) {
            continue;                          // blank line or SSE comment
        }
        char *payload = g_line + 5;
        while (*payload == ' ') {
            payload++;
        }
        if (strncmp(payload, "[DONE]", 6) == 0) {
            saw_done = true;
            break;
        }

        // Pull the base64 out by hand rather than parsing the delta as JSON.
        // A 24 KB line would need a JSON document of comparable size, doubling
        // peak memory in the one place we cannot afford it -- and the shape is
        // fixed and known.
        const char *key = strstr(payload, "\"data\":\"");
        if (key == nullptr) {
            continue;                          // role delta, transcript, usage
        }
        const char *b64 = key + 8;
        const char *close = strchr(b64, '"');
        if (close == nullptr) {
            Serial.println("[tts ] audio delta truncated -- line cap too small?");
            continue;
        }

        if (t_first == 0) {
            t_first = millis() - t0;
        }
        st.chunks++;
        if (!decode_b64_run(b64, close, sink)) {
            break;                             // stopped by a button
        }
    }

    sink.flush();
    http.end();

    st.samples = sink.samples;
    st.ms_to_first_audio = t_first;
    st.ms_total = millis() - t0;
    st.ok = saw_done && !sink.aborted && st.samples > 0;

    if (sink.has_odd) {
        // Not fatal, but it means the stream ended mid-sample.
        Serial.println("[tts ] stream ended on an odd byte; one byte dropped");
    }
    Serial.printf("[tts ] %s  first audio %lu ms, done %lu ms, %lu chunks, "
                  "%lu samples (%.2fs)\n",
                  st.ok ? "ok" : (sink.aborted ? "STOPPED" : "INCOMPLETE"),
                  (unsigned long)st.ms_to_first_audio,
                  (unsigned long)st.ms_total,
                  (unsigned long)st.chunks,
                  (unsigned long)st.samples,
                  st.samples / (float)TTS_SAMPLE_RATE);
    return st;
}
