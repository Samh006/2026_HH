#include "audio.h"

#include <Arduino.h>
#include <driver/i2s.h>

#include "buttons.h"
#include "config.h"

namespace {

constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

// Samples pushed per i2s_write() call. Small enough that a stop request is
// acted on within a few milliseconds, large enough not to thrash the driver.
// 512 samples at 24 kHz is ~21 ms of audio.
constexpr size_t CHUNK_SAMPLES = 512;

volatile bool g_stop = false;
bool g_running = false;

// Scratch for stereo duplication. Only allocated when the hardware turns out
// to need both channels.
int16_t g_stereo[CHUNK_SAMPLES * 2];

}  // namespace

bool audio_begin() {
    if (g_running) {
        return true;
    }

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = TTS_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    // The speaker is wired to one channel and we do not know which until the
    // hardware lane says so. ONLY_LEFT is the starting guess; if the bench
    // reports silence with the DAC visibly clocking, set
    // I2S_DUPLICATE_TO_STEREO to 1 in config.h and every sample goes to both.
#if I2S_DUPLICATE_TO_STEREO
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
#else
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
#endif
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 512;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;      // emit silence on underrun, not garbage

    esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_driver_install failed: %d\n", (int)err);
        return false;
    }

    i2s_pin_config_t pins = {};
    // MCLK only if the ES7148 turns out to need it. On the classic ESP32 the
    // master clock can only leave the chip on GPIO 0, 1 or 3, and 1/3 are the
    // serial port -- so GPIO 0 or nothing. Hardware resolves this on the bench
    // in the order: unconnected, then grounded, then driven from GPIO 0.
    pins.mck_io_num = (I2S_MCLK_PIN >= 0) ? I2S_MCLK_PIN : I2S_PIN_NO_CHANGE;
    pins.bck_io_num = I2S_BCK_PIN;
    pins.ws_io_num = I2S_LCK_PIN;
    pins.data_out_num = I2S_DIN_PIN;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_set_pin failed: %d\n", (int)err);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    i2s_zero_dma_buffer(I2S_PORT);
    g_running = true;
    g_stop = false;

    Serial.printf("[audio] i2s up: %d Hz 16-bit %s, bck=%d lck=%d din=%d mclk=%d\n",
                  TTS_SAMPLE_RATE,
                  I2S_DUPLICATE_TO_STEREO ? "stereo(dup)" : "mono(left)",
                  I2S_BCK_PIN, I2S_LCK_PIN, I2S_DIN_PIN, I2S_MCLK_PIN);
    return true;
}

void audio_end() {
    if (!g_running) {
        return;
    }
    i2s_zero_dma_buffer(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);
    g_running = false;
}

void audio_request_stop() { g_stop = true; }
void audio_clear_stop() { g_stop = false; }
bool audio_stop_requested() { return g_stop; }

void audio_stop_now() {
    g_stop = true;
    if (g_running) {
        i2s_zero_dma_buffer(I2S_PORT);
    }
}

bool audio_write(const int16_t *samples, size_t count) {
    if (!g_running || samples == nullptr) {
        return false;
    }

    size_t done = 0;
    while (done < count) {
        // A held button stops playback without waiting for the debounced,
        // classified event -- stopping has to feel instant.
        if (g_stop || buttons_any_down()) {
            g_stop = true;
            i2s_zero_dma_buffer(I2S_PORT);
            return false;
        }

        const size_t n = min(CHUNK_SAMPLES, count - done);
        const int16_t *src = samples + done;
        size_t bytes_in;
        const void *buf;

#if I2S_DUPLICATE_TO_STEREO
        for (size_t i = 0; i < n; i++) {
            g_stereo[2 * i] = src[i];
            g_stereo[2 * i + 1] = src[i];
        }
        buf = g_stereo;
        bytes_in = n * 2 * sizeof(int16_t);
#else
        buf = src;
        bytes_in = n * sizeof(int16_t);
#endif

        size_t written = 0;
        // portMAX_DELAY would block forever if the DAC stalls, taking the
        // watchdog with it. A bounded wait lets the stop check run again.
        const esp_err_t err =
            i2s_write(I2S_PORT, buf, bytes_in, &written, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            Serial.printf("[audio] i2s_write failed: %d\n", (int)err);
            return false;
        }
        if (written < bytes_in) {
            // Partial write: the DMA queue is full. Give it a moment rather
            // than spinning.
            delay(2);
        }
        done += (written / sizeof(int16_t)) >> (I2S_DUPLICATE_TO_STEREO ? 1 : 0);
    }
    return true;
}

bool audio_play(const int16_t *pcm, size_t count) {
    if (pcm == nullptr || count == 0) {
        // A missing phrase must be loud in the log but must not crash or hang.
        // Seven of ten phrases do not exist yet -- they are blocked on the
        // voice-cloning decision -- and the state machine has to run anyway.
        Serial.println("[audio] phrase missing or empty -- SILENCE (this is a bug "
                       "if it happens on a real failure path)");
        return true;
    }
    audio_clear_stop();
    const bool complete = audio_write(pcm, count);
    if (complete) {
        audio_drain();
    }
    return complete;
}

void audio_drain() {
    if (!g_running) {
        return;
    }
    // i2s_write of nothing does not flush, so wait out the DMA depth:
    // dma_buf_count * dma_buf_len samples at the sample rate, plus margin.
    const uint32_t queued_ms = (8UL * 512UL * 1000UL) / TTS_SAMPLE_RATE;
    delay(queued_ms + 20);
}
