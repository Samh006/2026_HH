# Hardware Brief — Talking Reader

**Owner: mechatronics engineer** · Read `00-TEAM-PLAN.md` first.

You own everything physical: power, wiring, the audio chain, the enclosure, and final assembly. The firmware track depends on you having a **bench rig that makes sound** by end of week 1 — that's your critical path.

---

## 1. What you have, and the one correction

| Part | What it actually is |
|---|---|
| Freenove Audio Converter & Amplifier | **ES7148 I²S DAC → PAM8403 Class-D amp.** VCC 3.3–5 V |
| 8 Ω 2 W speaker | Fine with the PAM8403, but see §4 on volume |
| Freenove ESP32-WROVER CAM | Primary board. OV2640 camera, USB-UART onboard |
| 2×AA holder, 9 V port | See §3 — we're using AAs, not the 9 V |
| Tactile buttons | Two, with **physically distinct caps** |

⚠️ **Correction to an earlier note:** I previously described this module as analog-input-only, based on a generic datasheet page. The ES7148 marking on your board settles it — it's a **24-bit stereo I²S DAC**. The module takes digital audio directly from the ESP32.

**This is good news: we do not need to buy a MAX98357A.** Saves $8, a part order, and a week-2 dependency.

### Shopping list (~$15)

- 1 × extra 2×AA battery holder (~$3) — series with the existing one → 4×AA
- 1 × 1000 µF 16 V electrolytic, 1 × 470 µF 16 V electrolytic (~$2)
- 2 × 0.1 µF ceramic (probably already in the kit)
- 4 × AA NiMH cells + charger, if not already on hand
- *Optional:* 1 × 4 Ω 3 W speaker as a louder backup (~$5) — only order this if §4's volume test disappoints

---

## 2. Pin map

The camera occupies most of the board. These are the pins left over, and the ones you must not touch.

### Assign these

| Function | GPIO | Notes |
|---|---|---|
| I²S **BCK** (bit clock) → module BCK | **32** | |
| I²S **LCK** (word clock) → module LCK | **33** | |
| I²S **DIN** (data) → module DIN | **14** | |
| I²S **MCLK** → module SCK | **0** | *Only if needed — see §4.* On classic ESP32, MCLK can only come out of GPIO 0, 1 or 3, and 1/3 are the serial port. |
| Button **A** (Read) | **13** | To GND, `INPUT_PULLUP` |
| Button **B** (Describe) | **15** | To GND, `INPUT_PULLUP`. Strapping pin, but pulling it low only silences boot logs — harmless. |

### Do not use

| GPIO | Why |
|---|---|
| **16, 17** | ⛔ Used internally by the WROVER module's PSRAM. Using these kills the camera framebuffer. This is the #1 mistake on WROVER boards. |
| 4, 5, 18, 19, 21, 22, 23, 25, 26, 27, 34, 35, 36, 39 | Camera bus |
| 34, 35, 36, 39 | Input-only anyway — no output, no pull-ups |
| **12** | Strapping pin, must be LOW at boot. A button with a pull-up here bricks the boot. Avoid. |
| 1, 3 | Serial / USB debug — you'll want these working |

### ✅ First task, 10 minutes

Open Freenove's pinout diagram for **your exact board revision** and confirm the camera pin list above. Revisions differ, and everything downstream depends on this table. Write the confirmed map into `hardware/wiring.md` and tell the firmware track.

---

## 3. Power

### Why not the 9 V port

Worth being able to explain, because someone will suggest it:

| | 9 V PP3 (rechargeable) | 4 × AA NiMH |
|---|---|---|
| Capacity | ~200 mAh | ~2000 mAh |
| Internal resistance | High | Low |
| Runtime at ~250 mA | **< 1 hour** | 4–6 hours |
| Regulator heat at 250 mA | ~1.14 W wasted | ~0.38 W |

The internal-resistance point is the one that actually bites. The ESP32 draws current in **spikes** when the Wi-Fi radio transmits. A PP3 sags under those spikes, the 3.3 V rail dips, and the chip browns out mid-upload. It presents as random resets and can eat days of debugging before anyone suspects the battery.

### The supply

```
  [2×AA holder] ── series ── [2×AA holder]        4 × NiMH ≈ 4.8 V
         │                          │
         └────────── + ─────────────┘
                     │
        ┌────────────┼─────────────┬──────────────┐
        │            │             │              │
     [1000 µF]   Board VIN/5V   [470 µF]   Audio module VCC
        │            │             │              │
       GND ─────────GND──────────GND────────────GND
```

**Both electrolytics matter.** The 1000 µF sits as close to the board's VIN pin as you can get it and absorbs Wi-Fi transmit spikes. The 470 µF sits at the audio module and stops speech playback from dragging the rail down. Watch the polarity — electrolytics fail loudly when reversed.

### The numbers

- **Voltage window:** fresh NiMH gives 4.8 V. The board's 3.3 V regulator is likely an AMS1117 with roughly 1.1 V of dropout, so VIN must stay above ~4.4 V. That means we use maybe 60–70% of the pack's capacity before brownout. Acceptable; know it's the tradeoff.
- **Current draw, expected:** ~200 mA idle with camera on, ~350 mA during Wi-Fi transmit, plus ~100–150 mA average during speech playback. Peaks near 500 mA.
- **Regulator heat:** (4.8 − 3.3) × 0.25 A ≈ **0.38 W**. It'll be warm, not hot. Don't bury it in foam.

*Optional upgrade if you have brownout trouble:* 5 × AA (6.0 V) gives a wider usable window at the cost of more regulator heat (~0.68 W). Try it only if 4×AA misbehaves.

### ⚖️ Weight — design around this now

**4 × AA NiMH weigh about 124 g.** Assembled, this device will be 280–320 g held up near someone's face. That is heavy for a one-handed device used by older users.

Two things follow, and they're your call to make early:

1. **Put the battery pack low in the grip**, so the mass sits in the palm rather than out at the camera end. A nose-heavy device is much more tiring to hold steady.
2. **Add a wrist strap anchor** to the CAD from v1. Cheap, and it's what stops a dropped device.

---

## 4. The audio chain

```
  ESP32 ──I²S── ES7148 DAC ── PAM8403 amp ── 8 Ω 2 W speaker
       BCK/LCK/DIN            (on the same module)
```

**Wiring:** module VCC → 4.8 V rail (not 3.3 V — you get more volume, and the module is rated 3.3–5 V). GND → common ground. BCK/LCK/DIN → GPIO 32/33/14.

### The SCK question — resolve this on the bench, day 2

The module exposes an **SCK** pin (master clock). Some I²S DACs need it; some generate their clock internally.

1. Try it first with **SCK unconnected**. If you get clean audio, you're done — leave GPIO 0 free.
2. Silence or noise? Ground SCK and retry.
3. Still nothing? Drive MCLK from **GPIO 0** and configure it in the I²S setup.

Work through it in that order and tell the firmware track which one worked. Don't let this become a mystery — it's a 20-minute test that otherwise turns into a two-day bug.

### Volume — test this in week 1

The PAM8403 delivers about 3 W into 4 Ω, but only **~1.5–2 W into your 8 Ω speaker**. Fine indoors; possibly marginal in a café or a busy demo hall.

**Test:** play speech at full volume, measure with a phone SPL meter at 0.5 m. You want **≥ 75 dB**. Then walk into the noisiest room you can find and see if it's still intelligible.

If it isn't, a 4 Ω speaker is a $5 fix — but only if you find out in week 1, not week 4. Also worth knowing: a small **sealed volume behind the speaker** in the printed body noticeably improves output. Design for it rather than leaving the driver open-backed.

---

## 5. Bench bring-up — in this order

Each step isolates one failure. Don't skip ahead; if step 3 fails you want to already know steps 1–2 are good.

| # | Test | Done when |
|---|---|---|
| 1 | Board alive — blink an LED over USB | LED blinks |
| 2 | Camera — flash Freenove's `CameraWebServer` example | Live stream in a browser |
| 3 | **Audio** — play a WAV from flash via I²S, no network | Recognisable speech from the speaker |
| 4 | Buttons — serial print on each press, debounced | Clean single events, no double-fires |
| 5 | Battery — repeat 2–4 on 4×AA instead of USB | All still work; log the voltage |
| 6 | Endurance — loop capture+playback until brownout | Runtime recorded |

Step 3 is your week-1 deliverable and the firmware track's dependency. **Prioritise it over everything else, including the enclosure.**

### 📋 Measurements to record

Fill this in and put it in `hardware/measurements.md`. These numbers go straight into the final writeup — judges like measured numbers.

| Measurement | Value |
|---|---|
| Idle current (camera on, Wi-Fi idle) | |
| Peak current during Wi-Fi transmit | |
| Current during audio playback | |
| Pack voltage — fresh | |
| Pack voltage — at brownout | |
| Runtime to brownout | |
| SPL at 0.5 m | |
| Assembled weight | |

---

## 6. Enclosure

### Grip before box

Print **three to five grip forms with no electronics in them at all** and get people to hold them, find the button, and mime using the device. Pick a winner, then design the shell around it.

The classic failure is a rectangular box that happens to fit the boards. You have the printers and the time — use week 2 for this, not week 4.

### 🔧 Refocus the lens — highest-value 5 minutes in the build

The OV2640's lens is **threaded and manually focusable**. From the factory it's set for a general scene, roughly infinity. Our device is used at **15–25 cm**.

Rotate the lens barrel to focus at reading distance and lock it with a dab of nail varnish or thread-lock. This costs nothing and will visibly improve text recognition. Do it in week 1, before anyone tunes prompts against blurry photos.

### Design requirements

| | |
|---|---|
| **Camera shroud** | Matte black PETG. Any gloss near the lens throws internal reflections that wash out text contrast. Never white. |
| **Lighting** | **Two** diffused white LEDs either side of the lens, not one — a single source casts a shadow across raised label text. A scrap of white PETG makes a fine diffuser. |
| **Button A** (Read) | Large, recessed in a tactile dish, at the natural thumb position. This is 90% of use. |
| **Button B** (Describe) | Smaller, different shape. Never two identical buttons side by side — they must be distinguishable by touch alone. |
| **Orientation rib** | A raised rib along the top edge so "which way is up" is unambiguous in the hand. |
| **Surface texture** | Print the back at 0.3 mm layer height for grip, the front at 0.12 mm for smoothness. They become instantly distinguishable by touch. |
| **Speaker** | Front-facing, aimed at the user. Grille holes, sealed volume behind. |
| **Battery hatch** | Openable **without tools** — AAs get swapped. A printed living hinge or a friction-fit door. |
| **Optional standoff** | Two small legs or a folding wire bail that set the exact focal distance when rested on a page. Crude, and it works. |

### Print settings

- **PETG, not PLA.** PLA softens around 60 °C, which a car in a Perth summer comfortably exceeds.
- 0.2 mm layers, 3 perimeters, 25% infill
- **Orient so layer lines run across the grip, not along it** — handhelds get dropped
- 2 mm fillets on every edge
- **Heat-set brass inserts** for every screw. Printed threads fail on the third disassembly, and you will open this many times.
- Recess a funnel around the USB port so a plug guides itself in by feel

---

## 7. Your week by week

**Week 1 — make it work on the bench.**
Confirm the pin map. Refocus the lens. Bench steps 1–4. Battery pack wired with capacitors. Volume tested.
✅ *Done: the board plays a WAV through the speaker on battery power, and two buttons report cleanly.*

**Week 2 — make it a shape.**
Grip forms printed and handled by real people. Shell v1 around the winner. Endurance test to brownout.
✅ *Done: a chosen grip geometry, a printed v1, and measured runtime.*

**Week 3 — make it an object.**
Final assembly, battery hatch, strap anchor, cable management, second unit started.
✅ *Done: a device someone can pick up and use unaided, in front of test users.*

**Week 4 — make it survive.**
🔒 Freeze day 22. Second unit finished. Drop-test the spare, not the primary. Spare AAs charged.
✅ *Done: two working units and charged batteries in a box.*

---

## 8. Tools and consumables

Soldering iron, solder, flux · heat-shrink · multimeter (**with current mode — you need it for §5**) · wire strippers · JST or Dupont crimps · heat-set insert tip for the iron · thread-lock or nail varnish · double-sided foam tape · zip ties · calipers

---

## 9. Things that will bite you

1. **GPIO 16/17.** They look free. They are not — they're the PSRAM bus. Using them breaks the camera in a way that looks like a software bug.
2. **Electrolytic polarity.** Reversed capacitors vent. Check twice.
3. **Common ground.** If the audio module and the board don't share a solid ground, you get noise or nothing. This is the most common I²S failure.
4. **Brownout that looks like a firmware bug.** If the board resets during upload, suspect power before code. Put the multimeter on the rail.
5. **Enclosure last.** Don't design the shell until the grip forms have been held by someone who isn't on the team.
6. **The demo unit is not the test unit.** Build the spare early and abuse that one.

---

*Questions on the I²S pin config or the firmware side → `02-SOFTWARE.md`.*
