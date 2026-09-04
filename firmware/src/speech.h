// speech.h -- text goes in, sound comes out of the speaker.
//
// This is the D14 path, NOT section 4 of the software brief. There is no
// /audio/speech endpoint on OpenRouter and no response_format: pcm; both
// return 400. Speech is a chat completion asked for the audio modality, and
// audio is refused unless stream is true. The bytes arrive base64-encoded
// inside SSE deltas at choices[0].delta.audio.data.
//
// tools/reference_pipeline.py tts() is the working reference and was written
// deliberately in the shape this file needs.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct SpeechStats {
    bool ok;
    uint32_t ms_to_first_audio;   // when the user actually hears something
    uint32_t ms_total;
    uint32_t chunks;
    uint32_t samples;
    int http_status;
};

// Speak `text`. Audio is pushed into I2S as it arrives -- nothing waits for
// the stream to finish, because buffering a whole utterance would be ~1 MB and
// would delay the first word for no benefit.
//
// Returns stats; check .ok. Interrupted playback (button pressed) returns
// ok == false with samples > 0.
SpeechStats speech_say(const char *text);
