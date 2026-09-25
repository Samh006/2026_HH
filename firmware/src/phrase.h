// phrase.h -- the offline voice. Every failure path speaks; silence is a bug.
//
// Seven of these ten do not exist yet -- they are blocked on the voice-cloning
// decision (D6/D13). phrase_play() logs a missing phrase loudly and returns,
// so the state machine can be written and tested against the full set today
// and the recordings drop in later without touching it.
//
// Earcons are synthesised in code rather than recorded: they are a click and
// two blips, they cost nothing to generate, and it means the shutter sound
// exists today rather than waiting on the same blocked decision.
#pragma once

#include <stdint.h>

enum PhraseId : uint8_t {
    PH_READY = 0,
    PH_READING,
    PH_DESCRIBING,
    PH_NO_TEXT,
    PH_NO_INTERNET,
    PH_CONNECTING,
    PH_BATT_LOW,
    PH_ERROR,
    PH_REPEATING,
    PH_UNCERTAIN,        // D17 -- "I am not certain of this..."

    // Recorded 25 Sep. These say WHICH failure happened, where PH_ERROR said
    // only that one had. Append-only, immediately before PH_COUNT: g_phrases[]
    // in phrase.cpp is a positional initialiser list, so inserting anywhere
    // else silently re-maps every phrase after it.
    PH_NO_CONNECTION,    // could not join Wi-Fi at all
    PH_SERVER_ERROR,     // Wi-Fi is fine; the reader did not answer, or errored
    PH_NO_AUDIO,         // answered, but there was no playable audio in it
    PH_OUT_OF_FOCUS,     // aiming: not at reading distance (ultrasonic)
    PH_UNHANDLED_ERROR,  // the genuine catch-all
    PH_NO_STORED_AUDIO,  // B pressed before anything has been read

    PH_COUNT,
};

// True if this phrase is actually recorded in this build. Lets a caller
// prefer a precise phrase and fall back to a blunter one that exists, instead
// of choosing between silence and saying the wrong thing.
bool phrase_available(PhraseId id);

// Plays the phrase, blocking. Missing phrases log and return immediately.
// Returns false if playback was interrupted by a button.
bool phrase_play(PhraseId id);

// Logs which phrases are missing. Call once at boot so the gap is visible in
// the serial log rather than discovered during a demo.
void phrase_report_missing();

// Earcons. 02-SOFTWARE.md section 7: the shutter click confirms the press
// physically and immediately, before any network work begins.
void earcon_shutter();
void earcon_tick();
void earcon_error();
