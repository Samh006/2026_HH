// buttons.h -- two buttons, four events.
//
// Button A (GPIO 13): short = Read, long = Summarise
// Button B (GPIO 15): short = Describe, long = Repeat
//
// Both are INPUT_PULLUP to GND, so a press reads LOW.
//
// Long press fires the moment the threshold is crossed, while the finger is
// still down, and the release is then swallowed. That is deliberate: a user
// who cannot see the device needs to know their long press registered without
// having to guess how long is long enough.
#pragma once

#include <stdint.h>

enum ButtonEvent : uint8_t {
    BTN_NONE = 0,
    BTN_A_SHORT,      // Read
    BTN_A_LONG,       // Summarise
    BTN_B_SHORT,      // Describe
    BTN_B_LONG,       // Repeat
};

void buttons_begin();

// Call every loop. Returns one event per press, BTN_NONE otherwise.
ButtonEvent buttons_poll();

// True while either button is physically down. The state machine uses this
// for the "any button stops playback" rule, which must not wait for a
// debounced, classified event -- stopping has to feel instant.
bool buttons_any_down();

const char *button_event_name(ButtonEvent e);
