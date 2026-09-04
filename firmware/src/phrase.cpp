#include "phrase.h"

#include <Arduino.h>
#include <math.h>

#include "audio.h"
#include "config.h"
#include "phrases.h"   // GENERATED -- defines HAVE_PHRASE_* for what exists

namespace {

struct Entry {
    const char *name;
    const int16_t *pcm;
    size_t len;
};

// HAVE_PHRASE_* comes from the generated header. A phrase that has not been
// recorded yet resolves to {nullptr, 0} and is reported, not silently skipped.
#define ENTRY(ID, NAME)                                     \
    {NAME, PHRASE_##ID, PHRASE_##ID##_LEN}
#define NO_ENTRY(NAME) {NAME, nullptr, 0}

const Entry g_phrases[PH_COUNT] = {
#ifdef HAVE_PHRASE_READY
    ENTRY(READY, "ready"),
#else
    NO_ENTRY("ready"),
#endif
#ifdef HAVE_PHRASE_READING
    ENTRY(READING, "reading"),
#else
    NO_ENTRY("reading"),
#endif
#ifdef HAVE_PHRASE_DESCRIBING
    ENTRY(DESCRIBING, "describing"),
#else
    NO_ENTRY("describing"),
#endif
#ifdef HAVE_PHRASE_NO_TEXT
    ENTRY(NO_TEXT, "no_text"),
#else
    NO_ENTRY("no_text"),
#endif
#ifdef HAVE_PHRASE_NO_INTERNET
    ENTRY(NO_INTERNET, "no_internet"),
#else
    NO_ENTRY("no_internet"),
#endif
#ifdef HAVE_PHRASE_CONNECTING
    ENTRY(CONNECTING, "connecting"),
#else
    NO_ENTRY("connecting"),
#endif
#ifdef HAVE_PHRASE_BATT_LOW
    ENTRY(BATT_LOW, "batt_low"),
#else
    NO_ENTRY("batt_low"),
#endif
#ifdef HAVE_PHRASE_ERROR
    ENTRY(ERROR, "error"),
#else
    NO_ENTRY("error"),
#endif
#ifdef HAVE_PHRASE_REPEATING
    ENTRY(REPEATING, "repeating"),
#else
    NO_ENTRY("repeating"),
#endif
#ifdef HAVE_PHRASE_UNCERTAIN
    ENTRY(UNCERTAIN, "uncertain"),
#else
    NO_ENTRY("uncertain"),
#endif
};

// Earcons are built on the fly into this buffer. 100 ms at 24 kHz is 2400
// samples; nothing here is longer than that.
constexpr size_t EARCON_MAX = 2400;
int16_t g_earcon[EARCON_MAX];

// A tone with a raised-cosine envelope. The envelope matters: a bare sine
// starting at full amplitude puts a step through the DAC, which is an audible
// click on top of the sound you wanted.
void tone(float freq_hz, uint32_t ms, float gain) {
    size_t n = (size_t)((uint64_t)TTS_SAMPLE_RATE * ms / 1000);
    if (n > EARCON_MAX) {
        n = EARCON_MAX;
    }
    for (size_t i = 0; i < n; i++) {
        const float t = (float)i / TTS_SAMPLE_RATE;
        const float env = 0.5f * (1.0f - cosf(2.0f * PI * i / (n - 1)));
        g_earcon[i] = (int16_t)(sinf(2.0f * PI * freq_hz * t) * env * gain * 32767.0f);
    }
    audio_clear_stop();
    audio_write(g_earcon, n);
}

}  // namespace

bool phrase_play(PhraseId id) {
    if (id >= PH_COUNT) {
        return true;
    }
    const Entry &e = g_phrases[id];
    if (e.pcm == nullptr || e.len == 0) {
        Serial.printf("[phr ] MISSING '%s' -- device is silent here. Record it "
                      "(blocked on the voice decision, D6/D13).\n", e.name);
        return true;
    }
    Serial.printf("[phr ] %s (%.2fs)\n", e.name, e.len / (float)TTS_SAMPLE_RATE);
    return audio_play(e.pcm, e.len);
}

void phrase_report_missing() {
    int missing = 0;
    Serial.print("[phr ] missing:");
    for (int i = 0; i < PH_COUNT; i++) {
        if (g_phrases[i].pcm == nullptr) {
            Serial.printf(" %s", g_phrases[i].name);
            missing++;
        }
    }
    if (missing == 0) {
        Serial.print(" none -- full phrase bank");
    } else {
        Serial.printf("   (%d of %d)", missing, (int)PH_COUNT);
    }
    Serial.println();
}

// Sharp and short -- this is the physical confirmation that the press landed,
// and it has to arrive at t=0, before anything slow starts.
void earcon_shutter() { tone(2200.0f, 25, 0.55f); }

// Quiet, once per second while waiting. Deliberately unobtrusive: it says
// "still working", not "look at me".
void earcon_tick() { tone(1400.0f, 18, 0.16f); }

void earcon_error() { tone(320.0f, 180, 0.45f); }
