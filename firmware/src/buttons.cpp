#include "buttons.h"

#include <Arduino.h>

#include "config.h"

namespace {

struct Button {
    uint8_t pin;
    ButtonEvent short_evt;
    ButtonEvent long_evt;

    // No default member initialisers here: the framework builds as C++11,
    // where those make the struct a non-aggregate and the brace-init below
    // stops compiling. Omitted members are value-initialised to 0/false.
    bool stable_down;            // debounced physical state
    bool raw_down;               // last raw sample
    uint32_t changed_at;         // when raw last flipped
    uint32_t pressed_at;         // when stable_down became true
};

Button g_buttons[] = {
    {BTN_A_PIN, BTN_A_SHORT, BTN_A_LONG},
    {BTN_B_PIN, BTN_B_SHORT, BTN_B_LONG},
};

constexpr size_t N_BUTTONS = sizeof(g_buttons) / sizeof(g_buttons[0]);

// Returns an event for this button, or BTN_NONE.
ButtonEvent update(Button &b, uint32_t now) {
    // INPUT_PULLUP: pressed == LOW.
    const bool raw = (digitalRead(b.pin) == LOW);

    if (raw != b.raw_down) {
        b.raw_down = raw;
        b.changed_at = now;                     // start the debounce window
        return BTN_NONE;
    }
    if (now - b.changed_at < BTN_DEBOUNCE_MS) {
        return BTN_NONE;                        // still bouncing
    }
    if (raw == b.stable_down) {
        return BTN_NONE;                        // steady, nothing to report
    }

    // Debounced edge.
    b.stable_down = raw;
    if (raw) {
        b.pressed_at = now;
        return BTN_NONE;                        // everything is decided on release
    }

    // Released, and this is the ONLY place an event is emitted. The long event
    // used to fire the moment the press crossed BTN_LONGPRESS_MS, while the
    // finger was still down -- and audio.cpp aborts playback whenever
    // buttons_any_down() is true, so a long press silenced its own
    // acknowledgement. Worse, the release was then swallowed, so holding
    // button B did nothing at all: the event fired into a muted speaker and
    // nothing else ever came.
    //
    // Deciding here costs the user nothing -- they find out what they did when
    // they let go, which is a few hundred milliseconds later -- and it makes
    // "B, however you press it" expressible at all.
    return (now - b.pressed_at >= BTN_LONGPRESS_MS) ? b.long_evt : b.short_evt;
}

}  // namespace

void buttons_begin() {
    const uint32_t now = millis();
    for (size_t i = 0; i < N_BUTTONS; i++) {
        pinMode(g_buttons[i].pin, INPUT_PULLUP);
        g_buttons[i].raw_down = (digitalRead(g_buttons[i].pin) == LOW);
        g_buttons[i].stable_down = g_buttons[i].raw_down;
        g_buttons[i].changed_at = now;
        // Seed this, do not leave it at 0. A pin reading LOW at boot -- held,
        // miswired, or shorted -- is seeded stable_down = true, and an
        // unseeded pressed_at of 0 would make the release edge compute a hold
        // time of however long the device has been running, i.e. always a long
        // press. Seeded, a button genuinely held through boot is simply
        // reported when it is let go.
        g_buttons[i].pressed_at = now;
    }
    // Buttons are GPIO 47 and 21 on this board (D20/D27). Neither is a
    // strapping pin, so a held button cannot stop it booting -- but 0, 45 and
    // 46 are, and hardware/wiring.md lists them as off limits for exactly
    // that reason.
}

ButtonEvent buttons_poll() {
    const uint32_t now = millis();
    for (size_t i = 0; i < N_BUTTONS; i++) {
        const ButtonEvent e = update(g_buttons[i], now);
        if (e != BTN_NONE) {
            return e;
        }
    }
    return BTN_NONE;
}

bool buttons_any_down() {
    for (size_t i = 0; i < N_BUTTONS; i++) {
        if (digitalRead(g_buttons[i].pin) == LOW) {
            return true;
        }
    }
    return false;
}

const char *button_event_name(ButtonEvent e) {
    switch (e) {
        case BTN_A_SHORT: return "A short  -> READ";
        case BTN_A_LONG:  return "A long   -> (unassigned)";
        case BTN_B_SHORT: return "B short  -> REPEAT";
        case BTN_B_LONG:  return "B long   -> REPEAT";
        default:          return "none";
    }
}
