#include "reader.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <string.h>

#include "audio.h"
#include "config.h"

namespace {

// Read buffer. Small on purpose: audio is pushed to I2S as it arrives, so
// there is never a reason to hold much of the utterance.
constexpr size_t NET_BUF = 1024;
constexpr size_t OUT_SAMPLES = 512;

uint8_t g_net[NET_BUF];
int16_t g_out[OUT_SAMPLES];

// What the body turned out to contain.
struct Format {
    uint32_t rate = TTS_SAMPLE_RATE;
    uint16_t channels = 1;
    uint16_t bits = 16;
    bool is_float = false;
};

const char *path_for(Mode m) {
    switch (m) {
        case MODE_SUMMARISE: return SERVER_SUMMARISE_PATH;
        case MODE_DESCRIBE:  return SERVER_DESCRIBE_PATH;
        default:             return SERVER_READ_PATH;
    }
}

// Block until `n` bytes are read or the deadline passes. Returns bytes read.
size_t read_exact(WiFiClient *s, uint8_t *dst, size_t n, uint32_t deadline) {
    size_t got = 0;
    while (got < n && millis() < deadline) {
        if (!s->connected() && s->available() == 0) {
            break;
        }
        const int r = s->read(dst + got, n - got);
        if (r > 0) {
            got += (size_t)r;
        } else {
            delay(1);
        }
    }
    return got;
}

// If the body starts with RIFF/WAVE, consume the header and fill `fmt` from
// the fmt chunk, leaving the stream positioned at the first audio byte.
// Returns false only on a malformed header; a body with no RIFF is fine and
// is treated as bare PCM in the format we expect.
bool sniff_wav(WiFiClient *s, Format *fmt, uint32_t deadline) {
    uint8_t hdr[12];
    if (read_exact(s, hdr, sizeof(hdr), deadline) != sizeof(hdr)) {
        return false;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        // Bare PCM. Those 12 bytes are audio -- push them so we do not clip
        // the first 6 samples off every utterance.
        int16_t lead[6];
        memcpy(lead, hdr, sizeof(hdr));
        audio_write(lead, 6);
        Serial.println("[read] body is bare PCM (no RIFF header)");
        return true;
    }

    // Walk chunks until 'data'. Everything before it is metadata.
    for (int guard = 0; guard < 16; guard++) {
        uint8_t ch[8];
        if (read_exact(s, ch, sizeof(ch), deadline) != sizeof(ch)) {
            return false;
        }
        const uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                            ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t f[40];
            const size_t want = sz > sizeof(f) ? sizeof(f) : sz;
            if (read_exact(s, f, want, deadline) != want) {
                return false;
            }
            const uint16_t tag = (uint16_t)f[0] | ((uint16_t)f[1] << 8);
            fmt->channels = (uint16_t)f[2] | ((uint16_t)f[3] << 8);
            fmt->rate = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
                        ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
            fmt->bits = (uint16_t)f[14] | ((uint16_t)f[15] << 8);
            fmt->is_float = (tag == 3);          // 3 = IEEE float, 1 = int PCM
            // Skip any remainder of an extended fmt chunk.
            for (uint32_t left = sz - want; left > 0;) {
                uint8_t junk[32];
                const size_t take = left > sizeof(junk) ? sizeof(junk) : left;
                if (read_exact(s, junk, take, deadline) != take) {
                    return false;
                }
                left -= take;
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            Serial.printf("[read] RIFF: %u Hz %u-bit %s %uch, %u bytes audio\n",
                          (unsigned)fmt->rate, (unsigned)fmt->bits,
                          fmt->is_float ? "float" : "int",
                          (unsigned)fmt->channels, (unsigned)sz);
            return true;                          // positioned at the audio
        } else {
            for (uint32_t left = sz + (sz & 1); left > 0;) {
                uint8_t junk[64];
                const size_t take = left > sizeof(junk) ? sizeof(junk) : left;
                if (read_exact(s, junk, take, deadline) != take) {
                    return false;
                }
                left -= take;
            }
        }
    }
    return false;
}

}  // namespace

ReadStats read_aloud(const uint8_t *jpeg, size_t len, Mode mode) {
    ReadStats st = {};
    st.ok = false;

    char url[192];
    snprintf(url, sizeof(url), "%s%s", SERVER_BASE_URL, path_for(mode));

    WiFiClient client;                    // plain HTTP on the LAN, no TLS
    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("[read] http.begin failed");
        return st;
    }
    http.addHeader("Content-Type", "image/jpeg");
    http.setTimeout(SERVER_TIMEOUT_MS);

    const uint32_t t0 = millis();
    Serial.printf("[read] POST %s (%u byte JPEG)\n", url, (unsigned)len);
    st.http_status = http.POST(const_cast<uint8_t *>(jpeg), len);

    if (st.http_status != HTTP_CODE_OK) {
        Serial.printf("[read] HTTP %d\n", st.http_status);
        http.end();
        return st;                        // caller maps the code to a phrase
    }

    WiFiClient *stream = http.getStreamPtr();
    const int content_len = http.getSize();
    const uint32_t deadline = millis() + SERVER_TIMEOUT_MS;

    Format fmt;
    audio_clear_stop();
    if (!sniff_wav(stream, &fmt, deadline)) {
        Serial.println("[read] could not read the audio header");
        http.end();
        return st;
    }
    if (fmt.rate != TTS_SAMPLE_RATE) {
        // No resampler on the device. Saying so beats playing it chipmunked
        // and leaving someone to guess why.
        Serial.printf("[read] ! server sent %u Hz, I2S is %d Hz -- speech will "
                      "play at the wrong speed\n",
                      (unsigned)fmt.rate, TTS_SAMPLE_RATE);
    }

    const size_t in_bytes = fmt.is_float ? 4 : (fmt.bits / 8);
    uint8_t carry[4];
    size_t carry_n = 0;
    uint32_t t_first = 0;
    bool stopped = false;

    while (!stopped && millis() < deadline) {
        if (!stream->connected() && stream->available() == 0) {
            break;
        }
        const int got = stream->read(g_net + carry_n, NET_BUF - carry_n);
        if (got <= 0) {
            delay(1);
            continue;
        }
        if (carry_n) {
            memcpy(g_net, carry, carry_n);    // carry is already at the front
        }
        size_t avail = carry_n + (size_t)got;

        // Whole samples only. The remainder is carried, so a sample is never
        // split across two i2s_write() calls -- that is the periodic click
        // 02-SOFTWARE.md 11.4 warns about.
        const size_t whole = (avail / in_bytes) * in_bytes;
        carry_n = avail - whole;

        size_t consumed = 0;
        while (consumed < whole && !stopped) {
            const size_t n = ((whole - consumed) / in_bytes) > OUT_SAMPLES
                                 ? OUT_SAMPLES
                                 : (whole - consumed) / in_bytes;
            for (size_t i = 0; i < n; i++) {
                const uint8_t *p = g_net + consumed + i * in_bytes;
                if (fmt.is_float) {
                    float f;
                    memcpy(&f, p, 4);
                    if (f > 1.0f) f = 1.0f;
                    if (f < -1.0f) f = -1.0f;
                    g_out[i] = (int16_t)(f * 32767.0f);
                } else {
                    g_out[i] = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                }
            }
            if (t_first == 0) {
                t_first = millis() - t0;
            }
            if (!audio_write(g_out, n)) {
                stopped = true;               // button pressed
            }
            st.samples += n;
            consumed += n * in_bytes;
        }
        if (carry_n) {
            memcpy(carry, g_net + whole, carry_n);
        }
    }

    http.end();
    st.ms_to_first_audio = t_first;
    st.ms_total = millis() - t0;
    st.ok = (st.samples > 0) && !stopped;

    Serial.printf("[read] %s  first audio %u ms, done %u ms, %u samples "
                  "(%.2fs)%s  content-length=%d\n",
                  st.ok ? "ok " : "INCOMPLETE", (unsigned)st.ms_to_first_audio,
                  (unsigned)st.ms_total, (unsigned)st.samples,
                  st.samples / (float)TTS_SAMPLE_RATE,
                  stopped ? "  [stopped by button]" : "", content_len);
    return st;
}
