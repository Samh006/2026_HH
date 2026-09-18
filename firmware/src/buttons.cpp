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
    bool long_fired;             // long event already emitted this press
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
        // Steady. The only thing that can happen is crossing into a long press.
        if (b.stable_down && !b.long_fired &&
            now - b.pressed_at >= BTN_LONGPRESS_MS) {
            b.long_fired = true;
            return b.long_evt;                  // fire while still held
        }
        return BTN_NONE;
    }

    // Debounced edge.
    b.stable_down = raw;
    if (raw) {
        b.pressed_at = now;
        b.long_fired = false;
        return BTN_NONE;                        // classify on release
    }
    // Released. If the long event already went out, swallow the release.
    return b.long_fired ? BTN_NONE : b.short_evt;
}

}  // namespace

void buttons_begin() {
    const uint32_t now = millis();
    for (size_t i = 0; i < N_BUTTONS; i++) {
        pinMode(g_buttons[i].pin, INPUT_PULLUP);
        g_buttons[i].raw_down = (digitalRead(g_buttons[i].pin) == LOW);
        g_buttons[i].stable_down = g_buttons[i].raw_down;
        g_buttons[i].changed_at = now;
        // Seed BOTH of these, and treat a pin that is already down as a press
        // we have already reported. They used to be left at 0/false, so a pin
        // reading LOW at boot -- held, miswired, or shorted -- was seeded
        // stable_down = true with pressed_at = 0, and update() then evaluated
        // `now - 0 >= BTN_LONGPRESS_MS` and emitted a LONG PRESS about 740 ms
        // into boot with nobody touching the device. main.cpp's swallow loop
        // then waited for a release that was never coming.
        g_buttons[i].pressed_at = now;
        g_buttons[i].long_fired = g_buttons[i].raw_down;
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
        case BTN_A_LONG:  return "A long   -> SUMMARISE";
        case BTN_B_SHORT: return "B short  -> DESCRIBE";
        case BTN_B_LONG:  return "B long   -> REPEAT";
        default:          return "none";
    }
}
