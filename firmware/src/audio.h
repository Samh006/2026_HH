// audio.h -- I2S output: phrases from flash, and streamed PCM from the cloud.
//
// Everything here is 16-bit signed mono at TTS_SAMPLE_RATE (24 kHz, confirmed
// in D15). That is what the phrase bank is, and what the TTS stream is, so no
// resampling happens anywhere on the device.
//
// STOPPING IS A FIRST-CLASS OPERATION. "Any button press during SPEAKING stops
// playback immediately" is called non-negotiable in 02-SOFTWARE.md section 7 --
// users must never feel trapped by their own device. So every blocking call
// here polls for a stop request between DMA chunks and returns false rather
// than running to completion.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Installs the I2S driver. False if the driver refused to install.
bool audio_begin();
void audio_end();

// Push mono 16-bit samples to the DAC. Blocks until written.
// Returns false if a stop was requested partway -- the caller should abandon
// whatever it was playing rather than continue.
bool audio_write(const int16_t *samples, size_t count);

// Play a phrase held in flash (see phrases.h). False if interrupted.
bool audio_play(const int16_t *pcm, size_t count);

// Block until the DMA buffers have drained, so we do not cut off the tail of
// an utterance by tearing the driver down or starting the next sound early.
void audio_drain();

// Silence the output immediately and discard anything still queued.
void audio_stop_now();

// Stop flag. audio_request_stop() is safe to call from anywhere; the playback
// loops check it. Clear it before starting a new sound.
void audio_request_stop();
void audio_clear_stop();
bool audio_stop_requested();
