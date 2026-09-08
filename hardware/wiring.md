# Wiring — bench rig

**Freenove ESP32-S3-WROOM CAM (N8R8), OV3660 camera.**

> **Rewritten 8 Sep 2026.** Every pin number in this file before today was for
> the **ESP32-WROVER**, the board we stopped using on 4 Sep (D20). It was still
> telling people to put the buttons on GPIO 13 and 15 — which are the camera's
> PCLK and XCLK on this board — and the I²S pins on GPIO 32/33, which do not
> exist on this header at all. `CLAUDE.md` points here as "the live pin map",
> so anyone who wired from it would have broken the camera and blamed software.
>
> `01-HARDWARE.md` §2 is the same stale map and has **not** been corrected.
> Treat this file as the pin map and that one as history.

Status: audio, buttons and Wi-Fi confirmed working on this board (4 Sep).
Camera pin map is **written but not yet confirmed on silicon** — see below.

---

## Confirmed pin map

These are the values live in `firmware/src/config.h` and proven booting.

| Function | GPIO | Confirmed |
|---|---|---|
| I²S bit clock → module **BCK** | **42** | ✅ 4 Sep, and again 8 Sep |
| I²S word clock → module **LCK** | **41** | ✅ |
| I²S data → module **DIN** | **40** | ✅ |
| I²S master clock → module **SCK** | **not connected** (`I2S_MCLK_PIN -1`) | ✅ see below |
| Button A (Read / Summarise) | **47** | ✅ 4 Sep, four distinct events |
| Button B (Describe / Repeat) | **21** | ✅ |

Boot log from 8 Sep, for reference:

```
[audio] i2s up: 24000 Hz 16-bit mono(left), bck=42 lck=41 din=40 mclk=-1
```

No resistors on the buttons — the firmware uses `INPUT_PULLUP`, so the internal
pull-up holds the pin HIGH and pressing pulls it to GND.

Give the two buttons **physically distinct caps.** They must be distinguishable
by touch alone; that is a requirement, not a nicety.

### Ultrasonic distance sensor — added 8 Sep

| Function | GPIO | |
|---|---|---|
| TRIG | **38** | free, no conflict |
| ECHO | **14** | free, no conflict |
| Supply | **3.3 V** | see note |

Checked against everything else claimed on this board — camera (4–18), I²S
(40/41/42), buttons (47/21), octal PSRAM (35/36/37). **Both pins are clear.**

⚠️ Note GPIO 14 was the I²S data pin in the *old WROVER* wiring, and the
pre-8-Sep version of this file still said so. It is free on the S3. If you find
a document claiming GPIO 14 is `DIN`, that document is for the other board.

**Running at 3.3 V, which is what makes this safe.** An HC-SR04 powered at 5 V
drives ECHO at 5 V, and the ESP32-S3 is not 5 V tolerant — that damages the pin
slowly rather than immediately, so it fails days later and looks like something
else. At a 3.3 V supply ECHO is 3.3 V and no divider is needed. **If anyone
ever moves this sensor to the 5 V rail, it needs a 1 kΩ/2 kΩ divider on ECHO.**
A plain HC-SR04 at 3.3 V is below its rated supply and may lose range; if
readings are short or erratic, that is the first thing to suspect.

**No firmware uses this yet.** Nothing reads either pin.

Why it is worth wiring: the user cannot see how far away they are holding the
device, and D18 makes 15–25 cm the primary accuracy lever. A distance reading
lets the device say "move closer" instead of silently photographing a blur.

### The two hardware questions, answered empirically

Both were open since day 2. Neither was ever tested deliberately, but the
device has now played audio correctly across two sessions with:

| Question | Working answer | Status |
|---|---|---|
| Does the ES7148 need MCLK on SCK? | **No** — runs with `mclk=-1`, SCK unconnected | Works. Never tested with SCK grounded or driven, so "no" is inferred from success, not measured. |
| Which channel is the speaker on? | **Left** — `mono(left)`, `I2S_DUPLICATE_TO_STEREO 0` | Works. Right was never tried. |

That is good enough to ship. Do not spend bench time re-testing these unless
audio actually breaks; if it does, these are the first two things to vary.

---

## Camera pin map — WRITTEN, NOT YET CONFIRMED

⚠️ **This is the ten-minute check this file has been asking for since day one,
and it still has not been done.** The map below is in
`firmware/src/camera.cpp` and the firmware builds and boots with it, but no
frame has been captured through it yet.

Two independent sources agree on it: Freenove's published pinout for the
S3-WROOM CAM, and `CLAUDE.md`'s note that "GPIO 13 and 15 are camera pins on
this board (PCLK and XCLK)". That is why it is worth trusting enough to flash —
but it is not the same as a photo.

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| PWDN | *(none)* | | D7 / Y9 | 16 |
| RESET | *(none)* | | D6 / Y8 | 17 |
| XCLK | **15** | | D5 / Y7 | 18 |
| SIOD (SCCB data) | 4 | | D4 / Y6 | 12 |
| SIOC (SCCB clock) | 5 | | D3 / Y5 | 10 |
| VSYNC | 6 | | D2 / Y4 | 8 |
| HREF | 7 | | D1 / Y3 | 9 |
| PCLK | **13** | | D0 / Y2 | 11 |

**How you will know it is wrong:** `esp_camera_init` returns
`ESP_ERR_NOT_FOUND` and `camera.cpp` prints *"sensor not detected — ribbon
seated? latch closed? pin map confirmed against THIS board revision?"*. A wrong
data pin usually gives a striped or half-green image rather than an init
failure.

To override without touching `camera.cpp`, define `CAM_PIN_PWDN` and the rest
in `config.h` — the defaults step aside as a block.

---

## Do not use these pins

Rebuilt for the S3. **The old version of this table was for the WROVER and got
three entries actively backwards on this board.**

| GPIO | Why |
|---|---|
| 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18 | **Camera bus.** Note 16/17 in particular — the old table called these the PSRAM bus and told you to avoid them for that reason. On this board they are camera data lines |
| **35, 36, 37** | ⛔ **Octal PSRAM bus** (D20). Using these kills the framebuffer, and it presents as a software bug |
| 19, 20 | USB D− / D+ — the OTG port |
| 43, 44 | UART0 TX / RX — the CH340/CH343 debug port, which you want working (D22) |
| 0, 45, 46 | Strapping pins. A pulled-up button here can stop the board booting |
| ~~32, 33~~ | **Do not exist on this header.** If a document tells you to use them, that document is for the WROVER |

**Free and in use by us:** 40, 41, 42 (I²S), 47 and 21 (buttons). 39 is free and
is where MCLK would go if the DAC ever needs it — on the S3 any GPIO can carry
it, unlike the classic ESP32 which was restricted to 0/1/3.

---

## Audio chain

`ESP32-S3 ──I²S── ES7148 DAC ── PAM8403 amp ── 8 Ω speaker`
(DAC and amp are both on the one Freenove module.)

| Wire | From | To | Note |
|---|---|---|---|
| Power | Board **5V / VIN** | module **VCC** | **Not 3.3 V.** Module is rated 3.3–5 V and 5 V gives more volume |
| Ground | Board **GND** | module **GND** | See the warning below — this one bites |
| Bit clock | **GPIO 42** | module **BCK** | |
| Word clock | **GPIO 41** | module **LCK** | |
| Data | **GPIO 40** | module **DIN** | |
| Master clock | *unconnected* | module **SCK** | Leave it. It works. |
| Speaker | module output | 8 Ω 2 W speaker | Left channel |

> ⚠️ **Common ground is the most frequent I²S failure.** If the board and the
> audio module do not share a solid ground you get noise, or nothing at all,
> and it looks exactly like a firmware bug. Check continuity with a multimeter
> before debugging anything else.

A **470 µF electrolytic across the module's VCC and GND** helps even on USB
power — speech playback pulls current in bursts and can drag the rail down.
**Check polarity twice.** Reversed electrolytics vent.

### Test audio, ready to go

`tools/phrases_pcm16/*.wav` — already 16-bit PCM at 24 kHz, exactly what the
I²S config expects. Do **not** use `Messages/*.wav`; those are 32-bit float and
play as full-scale noise (D7).

---

## Lens and lighting — the accuracy work, not polish

D18 measured every candidate vision model failing near-identically on a blurred
label and succeeding on a sharp one. **No cloud change fixes accuracy. These
two do.** Still outstanding as of 8 Sep.

- [ ] **Refocus the lens to 15–25 cm.** The barrel is threaded and factory-set
      near infinity. Rotate it to reading distance against a live stream, then
      lock it with nail varnish or thread-lock. Five minutes.
- [ ] **Two diffused white LEDs, one either side of the lens.** Not one — a
      single source shadows raised label text. A scrap of white PETG diffuses.

`firmware/src/camera_tuning.h` already fights motion blur in software (short
exposure, high gain, best-of-3 frames). That buys back some of what a blurry
lens loses; it does not replace focusing it.

---

## Battery (later, not today)

Stay on USB until audio and camera both work. The point of doing battery last
is that you already know the logic works before introducing a power variable.

Two 2×AA holders **in series** → 4×AA NiMH ≈ 4.8 V → board VIN, with
**1000 µF as close to the VIN pin as physically possible** to absorb Wi-Fi
transmit spikes.

**Never the 9 V PP3 port** (D3). If the board resets during an upload, suspect
power before code — put a multimeter on the rail.

---

## Measurements to record → `hardware/measurements.md`

All eight rows are still blank, and every one goes straight into the writeup.
Judges like measured numbers.

Idle current (camera on, Wi-Fi idle) · peak current on Wi-Fi transmit · current
during playback · pack voltage fresh · pack voltage at brownout · runtime to
brownout · SPL at 0.5 m (want ≥ 75 dB) · assembled weight.
