# Talking Reader — Team Plan

**Ahlulbayt Community of WA · Assistive tech hackathon · 4 weeks**
*Hand this to everyone. Hardware and software briefs are separate documents.*

---

## 1. What we're building

A handheld device with two buttons. Point it at a medicine label, a letter, a menu or a sign, press a button, and **it reads the text aloud**.

No screen. No phone. No app. One object, two buttons, a speaker.

### Who it's for

People with vision impairment who **can still aim a camera** and can hold something close to their face. Some can read text if it's right up close; some can't. All of them find small print on packaging, medication and mail a daily obstacle.

That user profile is a gift: the hardest problem in this field is helping someone aim a camera they can't see through. Our users can aim. So we spend our four weeks on **speed, reliability and feel** instead.

### Why not just a phone app

The honest answer, which we should say before a judge says it for us:

- Not everyone has a smartphone, and touchscreens are a real barrier for a lot of older users with vision loss
- No unlock, no app-switching, no menus — pick it up, press, listen. Two seconds of effort, not twenty
- Single-purpose, findable by touch, lives in one place
- Commercial dedicated readers cost thousands; ours is a kit plus about $15

We are going to **test this claim with real users in week 3**, not just assert it on a slide.

---

## 2. How it works

The ESP32 runs **no AI at all**. It takes a photo, uploads it, and plays back audio. Everything clever happens at OpenRouter, which gives us vision, speech and transcription behind one API key.

```
 [Button] → [Camera] → [base64 + POST] ──▶ OpenRouter /chat/completions
                                                      │ transcribed text
                                          ┌───────────┘
                                          ▼
 [Speaker] ◀── [I²S DAC+amp] ◀── PCM ── OpenRouter /audio/speech
```

Two cloud round trips per press. Roughly **4–9 seconds to the first spoken word**.

See `architecture.png` for the full diagram.

### What works with no internet

This matters, because a device that goes silent when Wi-Fi drops feels broken:

- **All system speech** — "Ready", "No internet", "Nothing found", "Battery low" — is pre-recorded and baked into flash memory. The device always talks.
- Earcons: shutter click, waiting tick, error tone
- "Repeat last" replays the previous result from cache
- QR codes decode fully on-device

---

## 3. Our parts — confirmed

| Item | Notes |
|---|---|
| Freenove ESP32-WROVER CAM board | **Primary board.** OV2640 2 MP camera, built-in USB-UART |
| Seeed XIAO ESP32S3 Sense | Backup board; has a microphone if we do voice commands |
| Freenove Audio Converter & Amplifier | **ES7148 I²S DAC + PAM8403 amp**, VCC 3.3–5 V |
| 8 Ω 2 W speaker | Matches the amp; check volume early |
| Tactile buttons | Two, with physically distinct caps |
| 2×AA battery holder | See power decision below |
| OpenRouter API key | ✅ Ready |

**Still to buy — about $15 total:**

- 1 × extra 2×AA battery holder (~$3) — wired in series with the one we have, giving 4×AA
- 1 × 1000 µF and 1 × 470 µF electrolytic capacitor (~$2)
- 1 × 4 Ω speaker as a louder backup (~$5), optional
- 4 × rechargeable AA NiMH cells if we don't already have them

That's it. Order this week.

---

## 4. Decisions already made

Recorded so nobody relitigates them in week 3. Add to this list as we go — it becomes our writeup for free.

| # | Decision | Why |
|---|---|---|
| D1 | **Freenove WROVER-CAM is the primary board** | It's the kit board, has built-in USB-UART so all four of us can flash it, and has plenty of spare GPIO. Since we're on AA cells, the XIAO's onboard LiPo charging is no longer an advantage. |
| D2 | **No MAX98357A needed** | Our audio module is an **ES7148 — a 24-bit I²S DAC** — feeding a PAM8403 amp. It takes I²S digital audio directly. *(This corrects an earlier note that called it analog-only; the ES7148 marking settles it.)* |
| D3 | **4×AA NiMH into VIN — not the 9 V port** | See §5. |
| D4 | **Speech only, no screen** | The kit's only display is a 16×2 character LCD. Not worth fighting. |
| D5 | **OpenRouter for vision + TTS, `response_format: pcm`** | PCM goes straight into the I²S buffer with no MP3 decoder. Saves days. |
| D6 | **System phrases pre-recorded to flash** | Generated on a laptop with pocket-tts, voice-cloned to match our cloud TTS voice so it's one voice throughout. |

---

## 5. Power — use the AAs, not the 9 V

This is worth understanding as a team because it's the most common way an ESP32 camera project dies.

**Don't use the 9 V PP3 port.** Concretely:

- A rechargeable 9 V PP3 holds roughly **200 mAh**. Four AA NiMH hold **2000 mAh** — ten times more.
- PP3 cells have high internal resistance. The ESP32 draws current in spikes when the Wi-Fi radio transmits; the pack voltage dips and the board browns out mid-upload. This failure looks like random resets and will eat days of debugging.
- Dropping 9 V to 3.3 V in a linear regulator throws away about 63% of the energy as heat.

**Do this instead:** wire the two 2×AA holders in series → **4 × AA NiMH ≈ 4.8 V** → the board's VIN/5 V pin. Add a 1000 µF capacitor across the supply to absorb the Wi-Fi current spikes.

Expected runtime: **4–6 hours** of intermittent use. Plenty for a demo day and a user pilot.

Full calculations and wiring in the hardware brief.

---

## 6. Who does what

Four people, two tracks, meeting at a written interface so neither waits on the other.

| Track | People | Owns |
|---|---|---|
| **Hardware** | Mechatronics engineer | Power, wiring, audio bench-up, enclosure and CAD, assembly |
| **Firmware** | Computer engineer (+1 SWE pairing) | Camera, Wi-Fi/TLS, the uploader, I²S playback, buttons, state machine |
| **Cloud & tooling** | 2 software engineers | Reference pipeline, prompts, eval set, mock server, phrase generation |

**Plus two named roles — assign these in week 1 or they won't happen:**

- **Demo owner** — the pitch, the poster, the five-minute script. Starts week 1, not week 4.
- **User-testing owner** — books the testers, runs the sessions, writes up what happened.

### The interface between tracks

Firmware develops against a **local mock server** that speaks OpenRouter's shape, not against OpenRouter itself. The cloud pair owns that mock.

This is the most important structural decision in the plan: firmware work becomes deterministic and free, nobody waits for prompts to be finished, and **if the venue Wi-Fi dies on demo day we point the device at a laptop and still demo.**

---

## 7. Timeline

| | Milestone — something you can demonstrate |
|---|---|
| **Week 1** | Press a button on a breadboard → photo taken → posted to the mock server → **audio comes out of the speaker.** Prompts finalised against 20 real photos. System phrases baked into flash. |
| **Week 2** | Swap mock for real OpenRouter. HTTPS working. All three modes. Latency, cost and accuracy measured on real objects. Enclosure v1 printed. |
| **Week 3** | Battery, final assembly, graceful failure on every path. **Put it in front of 3–4 people with vision loss.** |
| **Week 4** | 🔒 **Feature freeze day 22.** Fix what testing found. Build a spare unit. Rehearse the demo three times. |

**If week 1's milestone isn't met by day 7, we cut** — in this order: voice commands → Mode C (summarise) → Mode B (describe). **Mode A alone, working reliably, beats four flaky modes.** Agreeing this now means nobody gets precious about it later.

---

## 8. The three modes

| Control | Mode | What it does |
|---|---|---|
| **A** short press | **Read** | Speaks the text verbatim. This is 90% of use. |
| **A** long press | **Summarise** | "This is a gas bill. $184, due 12 September." |
| **B** short press | **Describe** | Describes the scene, not the text. |
| **B** long press | **Repeat** | Replays the last result. Works offline. |
| Any button while speaking | **Stop** | Instant. Non-negotiable. |

**Filling the silence** is a design requirement, not a nicety. Five to ten seconds of nothing reads as "broken" and users press again, queueing a second paid API call. So: shutter click on press, "Reading" from flash, a quiet tick once per second while waiting, then speech.

---

## 9. Risks

| Risk | What we do about it |
|---|---|
| **Venue Wi-Fi / captive portals** | Two phone hotspots, credentials hardcoded with fallback. Mock server as a demo backup. Test on the actual hotspot in week 2. |
| **Brownout on Wi-Fi transmit** | 4×AA and the smoothing capacitors (§5). Log free heap and voltage from day one. |
| **Speaker too quiet in a noisy room** | Test volume in week 1. A 4 Ω speaker is a $5 fix, but only if we find out early. |
| **Integration left to the end** | Mock server; integrate daily; never let the tracks diverge more than 24 hours. |
| **Nobody talks to a user** | Testers booked in week 1 for week 3. Named owner. |
| **API key sits in flash** | True and unavoidable here. We name it as a known limitation and describe the production fix, rather than getting caught by it. |

---

## 10. Working agreements

- **15-minute standup daily**, same time. What shipped, what's blocking, has the eval score moved.
- **One decision log** — `docs/decisions.md`, one dated line per decision.
- **Feature freeze day 22.** Write the date on the wall.
- **Integrate every day.** The classic hackathon death is two halves that each work and have never met.
- **`config.h` is gitignored from commit one.** Nobody pushes the API key.

---

## 11. This week

- [ ] Order: extra 2×AA holder, capacitors, AA NiMH cells, backup 4 Ω speaker
- [ ] Book 3–4 test users for week 3
- [ ] Repo created, structure pushed, `config.h` gitignored
- [ ] Tracks assigned; demo owner and user-testing owner named
- [ ] Day 1, together: flash Freenove's **CameraWebServer** example and see the stream in a browser — proves board, camera, USB and Wi-Fi in 20 minutes
- [ ] Day 1: everyone makes one `curl` call to OpenRouter with an image
- [ ] Feature freeze date on the wall

---

## 12. Glossary

Because we're four people from three disciplines.

| Term | Meaning |
|---|---|
| **I²S** | The digital audio wiring standard between a chip and a DAC. Three signal wires: bit clock, word clock, data. |
| **DAC** | Digital-to-analog converter. Turns numbers into a voltage a speaker can use. Ours is the ES7148. |
| **PCM** | Raw uncompressed audio samples. No decoding needed — the whole reason we can do audio on a microcontroller. |
| **PSRAM** | Extra RAM on the ESP32 module. Where the camera photo lives. |
| **VLM** | Vision-language model. The cloud model that reads our photos. |
| **Earcon** | A short sound that conveys state — a click, a tick, an error tone. |
| **Brownout** | Supply voltage sags below what the chip needs and it resets. Our main power risk. |
| **Mock server** | A fake OpenRouter on a laptop, so firmware can be developed without internet or cost. |

---

*Companions: `01-HARDWARE.md`, `02-SOFTWARE.md`, `architecture.png`, `esp32-reader-build-spec.md`.*
