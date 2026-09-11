#include "reader.h"

#include <Arduino.h>
#include <string.h>

#include "audio.h"
#include "config.h"

namespace {

// Conversion scratch. There is no network buffer any more -- the caller owns
// the body -- so this is the only buffer left in the file.
constexpr size_t OUT_SAMPLES = 512;
int16_t g_out[OUT_SAMPLES];

struct Format {
    uint32_t rate = TTS_SAMPLE_RATE;
    uint16_t channels = 1;
    uint16_t bits = 16;
    bool is_float = false;
    size_t offset = 0;            // index of the first audio byte
};

uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Fill `fmt` and point fmt->offset at the first audio byte. A body with no
// RIFF magic is not an error -- it is bare PCM in the format we expect.
bool parse_wav(const uint8_t *b, size_t len, Format *fmt) {
    if (len < 12 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) {
        fmt->offset = 0;
        Serial.println("[play] bare PCM (no RIFF header)");
        return true;
    }

    size_t p = 12;
    while (p + 8 <= len) {
        const uint8_t *id = b + p;
        const uint32_t sz = le32(b + p + 4);
        p += 8;

        if (memcmp(id, "fmt ", 4) == 0 && p + 16 <= len) {
            const uint16_t tag = le16(b + p);
            fmt->channels = le16(b + p + 2);
            fmt->rate     = le32(b + p + 4);
            fmt->bits     = le16(b + p + 14);
            fmt->is_float = (tag == 3);        // 3 = IEEE float, 1 = int PCM
        } else if (memcmp(id, "data", 4) == 0) {
            fmt->offset = p;
            Serial.printf("[play] RIFF: %u Hz %u-bit %s %uch, %u bytes "
                          "declared\n", (unsigned)fmt->rate,
                          (unsigned)fmt->bits, fmt->is_float ? "float" : "int",
                          (unsigned)fmt->channels, (unsigned)sz);
            return true;                       // positioned at the audio
        }
        p += sz + (sz & 1);                    // RIFF chunks are word-aligned
    }

    Serial.println("[play] RIFF header but no data chunk -- nothing to play");
    return false;
}

}  // namespace

ReadStats read_aloud(const uint8_t *body, size_t len) {
    ReadStats st = {};
    const uint32_t t0 = millis();

    if (body == nullptr || len == 0) {
        // Reached on HTTP 200 with an empty body -- a real case, because a
        // chunked reply gives client.cpp no Content-Length to size its
        // buffer from. The caller speaks a phrase; it is never silent.
        Serial.println("[play] empty body -- nothing to play");
        return st;
    }

    Format fmt;
    if (!parse_wav(body, len, &fmt)) {
        return st;
    }

    // Reject what we cannot convert rather than mangling it. The streaming
    // version derived a stride from `bits` but then always read two bytes,
    // so an 8- or 24-bit body played as noise with nothing logged.
    const size_t in_bytes = fmt.is_float ? 4 : (size_t)(fmt.bits / 8);
    if (!fmt.is_float && fmt.bits != 16) {
        Serial.printf("[play] cannot play %u-bit PCM -- only 16-bit int or "
                      "32-bit float\n", (unsigned)fmt.bits);
        return st;
    }
    if (fmt.channels == 0) {
        Serial.println("[play] header claims zero channels -- refusing");
        return st;
    }

    if (fmt.rate != TTS_SAMPLE_RATE) {
        // No resampler on the device. Saying so beats playing it chipmunked
        // and leaving someone to guess why.
        Serial.printf("[play] ! server sent %u Hz, I2S is %d Hz -- speech will "
                      "play at the wrong speed\n",
                      (unsigned)fmt.rate, TTS_SAMPLE_RATE);
    }
    if (fmt.channels != 1) {
        // `channels` used to be parsed and then never used, so a stereo body
        // was read as though every sample were mono -- full-scale noise at
        // double rate, with no error anywhere. Taking the left channel is
        // wrong-ish but audible and diagnosable; the old behaviour was
        // neither.
        Serial.printf("[play] ! %u channels -- taking left only\n",
                      (unsigned)fmt.channels);
    }

    const uint8_t *pcm = body + fmt.offset;
    const size_t pcm_bytes = len - fmt.offset;
    const size_t frame_bytes = in_bytes * fmt.channels;
    // Trust the buffer we were actually handed, not the size in the header:
    // a streamed WAV often declares 0 or 0xFFFFFFFF for its data chunk.
    const size_t frames = pcm_bytes / frame_bytes;

    if (frames == 0) {
        Serial.printf("[play] %u bytes of audio is less than one frame\n",
                      (unsigned)pcm_bytes);
        return st;
    }

    audio_clear_stop();
    bool stopped = false;
    uint32_t t_first = 0;
    size_t done = 0;

    // The one-byte sample carry is gone, and this is why: it existed only
    // because TCP reads land on arbitrary boundaries, so a 16-bit sample
    // could be split across two i2s_write() calls -- the periodic click
    // 02-SOFTWARE.md 11.4 warns about. The body is one contiguous buffer
    // now, so there are no boundaries left to straddle.
    while (done < frames && !stopped) {
        const size_t n = (frames - done) > OUT_SAMPLES ? OUT_SAMPLES
                                                       : (frames - done);
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p = pcm + (done + i) * frame_bytes;   // left channel
            if (fmt.is_float) {
                float f;
                memcpy(&f, p, 4);
                if (f > 1.0f) f = 1.0f;
                if (f < -1.0f) f = -1.0f;
                g_out[i] = (int16_t)(f * 32767.0f);
            } else {
                g_out[i] = (int16_t)le16(p);
            }
        }
        if (t_first == 0) {
            t_first = millis() - t0;
        }
        if (!audio_write(g_out, n)) {
            stopped = true;                    // button pressed
        }
        st.samples += n;
        done += n;
    }

    st.ms_to_first_audio = t_first;
    st.ms_total = millis() - t0;
    st.ok = (st.samples > 0) && !stopped;

    Serial.printf("[play] %s  %u samples (%.2fs) in %u ms%s\n",
                  st.ok ? "ok " : "INCOMPLETE", (unsigned)st.samples,
                  st.samples / (float)TTS_SAMPLE_RATE,
                  (unsigned)st.ms_total,
                  stopped ? "  [stopped by button]" : "");
    return st;
}
