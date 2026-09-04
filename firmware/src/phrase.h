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
    PH_COUNT,
};

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
