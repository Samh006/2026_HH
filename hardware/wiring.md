# Wiring — bench rig

**Freenove ESP32-WROVER CAM.** This is the file `01-HARDWARE.md` §2 asks for.
Consolidated here because the pin map, the audio chain and the bring-up order
live in three different sections of two different documents.

Status: camera confirmed working (bench step 2 ✅). Everything below is still
to do.

> ⚠️ **Confirm the camera pin list against your exact board revision** before
> trusting the "do not use" table. Freenove revisions differ. Open their pinout
> diagram for the board you physically have and check it. Everything downstream
> depends on this and it is 10 minutes.

---

## Stage 1 — Buttons only (10 minutes)

Do this first. It unblocks a full firmware test with no audio hardware at all:
the state machine runs, joins Wi-Fi, POSTs to the mock, decodes the audio
stream and logs the heap. It simply stays silent.

| Wire | From | To |
|---|---|---|
| Button A (Read) | **GPIO 13** | one leg → GPIO 13, other leg → GND |
| Button B (Describe) | **GPIO 15** | one leg → GPIO 15, other leg → GND |

No resistors needed — the firmware uses `INPUT_PULLUP`, so the internal pull-up
holds the pin HIGH and pressing pulls it to GND.

Give the two buttons **physically distinct caps**. They must be
distinguishable by touch alone; that is a requirement, not a nicety.

**Test:** flash, open the serial monitor at 115200, press each button short and
long. You want four distinct events and no double-fires.

```
[btn ] A short  -> READ
[btn ] A long   -> SUMMARISE
[btn ] B short  -> DESCRIBE
[btn ] B long   -> REPEAT
```

GPIO 15 is a strapping pin. Held LOW at boot it only silences the ROM boot log,
which is harmless. But if the board refuses to boot with the button wired,
that is the first thing to suspect.

---

## Stage 2 — Audio (30–45 minutes)

`ESP32 ──I²S── ES7148 DAC ── PAM8403 amp ── 8 Ω speaker`
(DAC and amp are both on the one Freenove module.)

| Wire | From | To | Note |
|---|---|---|---|
| Power | Board **5V / VIN** | module **VCC** | **Not 3.3 V.** Module is rated 3.3–5 V and 5 V gives more volume |
| Ground | Board **GND** | module **GND** | See the warning below — this one bites |
| Bit clock | **GPIO 32** | module **BCK** | |
| Word clock | **GPIO 33** | module **LCK** | |
| Data | **GPIO 14** | module **DIN** | |
| Master clock | **GPIO 0** *(maybe)* | module **SCK** | Resolve with the procedure below |
| Speaker | module output | 8 Ω 2 W speaker | |

> ⚠️ **Common ground is the most frequent I²S failure.** If the board and the
> audio module do not share a solid ground you get noise, or nothing at all,
> and it looks exactly like a firmware bug. Check it with a multimeter for
> continuity before you debug anything else.

A **470 µF electrolytic across the module's VCC and GND** helps even on USB
power — speech playback pulls current in bursts and can drag the rail down.
**Check polarity twice.** Reversed electrolytics vent.

### The SCK question — 20 minutes, in this exact order

Some I²S DACs need a master clock, some generate it internally. Work through
these in order and **tell the firmware track which one worked** — it is a
one-constant change in `config.h`, and a two-day bug if left a mystery.

1. **SCK unconnected.** Flash and play a phrase. Clean audio? Done, and GPIO 0
   stays free. Leave `I2S_MCLK_PIN -1`.
2. Silence or noise? **Ground SCK** and retry. Still `I2S_MCLK_PIN -1`.
3. Still nothing? **Wire SCK to GPIO 0** and set `I2S_MCLK_PIN 0` in
   `firmware/src/config.h`.

On the classic ESP32 the master clock can only come out of GPIO 0, 1 or 3 —
and 1 and 3 are the serial port you need for debugging. Hence GPIO 0.

### Which channel is the speaker on?

The other unknown the firmware needs answered. If the DAC is clearly clocking
but you hear silence, the speaker is probably on the channel we are not
sending to. Set `I2S_DUPLICATE_TO_STEREO 1` in `config.h` and retry.

→ Record both answers here when known:

| Question | Answer | Date |
|---|---|---|
| Does the ES7148 need MCLK on SCK? | | |
| Speaker channel — left / right / both? | | |

### Test audio, ready to go

`tools/phrases_pcm16/*.wav` — already 16-bit PCM at 24 kHz, which is exactly
what the I²S config expects. Do **not** use `Messages/*.wav`; those are 32-bit
float and will play as full-scale noise (D7).

---

## Stage 3 — Battery (later, not today)

Stay on USB until stages 1 and 2 both work. Bench step 5 in the hardware brief
is "repeat 2–4 on battery" — the point is that you already know the logic works
before you introduce a power variable.

When you get there: two 2×AA holders **in series** → 4×AA NiMH ≈ 4.8 V → board
VIN, with **1000 µF as close to the VIN pin as physically possible** to absorb
Wi-Fi transmit spikes.

**Never the 9 V PP3 port** (D3). And if the board resets during an upload,
suspect power before code — put a multimeter on the rail.

---

## Do not use these pins

| GPIO | Why |
|---|---|
| **16, 17** | ⛔ PSRAM bus on the WROVER module. Using these kills the camera framebuffer. The #1 mistake on this board, and it presents as a software bug |
| **12** | Strapping pin, must be LOW at boot. A button with a pull-up here stops the board booting |
| 4, 5, 18, 19, 21, 22, 23, 25, 26, 27, 34, 35, 36, 39 | Camera bus |
| 34, 35, 36, 39 | Input-only regardless — no output, no pull-ups |
| 1, 3 | Serial / USB debug, which you want working |

---

## Firmware constants these map to

`firmware/src/config.h` — change the constant, do not rewire to match the code.

```c
#define I2S_BCK_PIN     32
#define I2S_LCK_PIN     33
#define I2S_DIN_PIN     14
#define I2S_MCLK_PIN    -1   // -> 0 only if SCK step 3 was needed
#define BTN_A_PIN       13
#define BTN_B_PIN       15
#define TTS_SAMPLE_RATE 24000
```

---

## Measurements to record → `hardware/measurements.md`

All eight rows are currently blank, and every one goes straight into the
writeup. Judges like measured numbers.

Idle current (camera on, Wi-Fi idle) · peak current on Wi-Fi transmit · current
during playback · pack voltage fresh · pack voltage at brownout · runtime to
brownout · SPL at 0.5 m (want ≥ 75 dB) · assembled weight.
