# Today — 4 Sep 2026 · full team present

**Day 1 of 11.** Ships 14 Sep. Freeze 12 Sep.

Cloud tooling is done and ahead. **Firmware is zero lines and hardware has zero
recorded measurements.** Today exists to change both of those, and to make the
four decisions that are currently blocking other people's work.

> **Today's win condition:** the board takes a photo and speech comes out of the
> speaker — even if those are two separate demos on the same bench.

---

## Part 1 — First 90 minutes, everyone together

These are the "Day 1, together" items from `00-TEAM-PLAN.md` §11 and
`02-SOFTWARE.md` §2. None have been done, and three of them block other people.
**Do these before splitting up.**

- [ ] **PlatformIO installed and working on all four machines.** Not the Arduino
      IDE — `02-SOFTWARE.md` §1 explains why (global library folder = "works on
      my machine" bugs you can't afford in 11 days).
- [ ] **Everyone flashes Freenove's `CameraWebServer` example and sees the
      stream in a browser.** 20 minutes, and it proves board + camera + USB +
      Wi-Fi all at once. It also means all four of you can flash a board, which
      matters when one person is stuck.
- [ ] 🔴 **Confirm the camera pin map against your exact board revision.**
      Open Freenove's pinout diagram for the revision you physically have and
      check it against the table in `01-HARDWARE.md` §2. **Revisions differ and
      everything downstream depends on this.** Write the confirmed map into
      `hardware/wiring.md` and tell the firmware track.
- [ ] 🔴 **Refocus the lens.** The OV2640 barrel is threaded and set near
      infinity from the factory. You use this at 15–25 cm. Rotate it to reading
      distance, check against the browser stream from the step above, lock it
      with nail varnish or thread-lock. **Five minutes. D18 makes this a primary
      accuracy mechanism, not polish** — do it before anyone tunes a prompt or
      shoots an eval photo against a blurry image.
- [ ] **Everyone makes one `curl` to OpenRouter with an image** and sees text
      come back. Cheap, and it means nobody's first encounter with the API is
      during a bug hunt.
- [ ] **Copy `config.h` on every machine** and put the laptop's LAN IP in it:
      `cp firmware/include/config.example.h firmware/src/config.h`

---

## Part 2 — Four decisions, while you're all in one room

These take fifteen minutes of talking and each one is blocking real work.
Write the outcome into `docs/decisions.md` as D20–D23 before you split.

- [ ] 🔴 **Voice cloning: Route A or Route B?** `kyutai/pocket-tts` is gated —
      it needs its HF terms accepted plus a local login, otherwise it silently
      falls back to 26 fixed voices with no cloning.
      **A:** accept the terms, keep D6's one-voice-throughout goal.
      **B:** pick a catalog voice for the offline phrases, then choose the
      closest-sounding OpenRouter voice — this reverses D13's ordering.
      *Blocks 7 of 10 phrases, including the D17 "I am not certain" phrase.*
- [ ] 🔴 **Name the demo owner and the user-testing owner.** The plan's own
      words: named in week 1 or they don't happen.
- [ ] 🔴 **Book 3–4 test users for day 9 (12 Sep).** Phone calls, today. Booked
      later than today and they probably can't make the date — and user testing
      is what turns "a phone app is worse for these users" from an assertion
      into evidence.
- [ ] **Confirm the cut list.** Ship **Mode A (Read) + Repeat**. Cut voice
      commands outright; cut Modes B and C from the plan but note they're a
      prompt string on an identical code path — re-add on day 10 if the freeze
      is calm. Agreeing this now stops anyone getting precious about it on day 8.

---

## Part 3 — Hardware lane

**Owner: mechatronics.** Parts are available today, so the battery work happens
today rather than waiting.

Work through `01-HARDWARE.md` §5 in order — each step isolates one failure, so
if step 3 fails you already know 1 and 2 are good.

### The one that matters

- [ ] 🔴 **Bench step 3 — play a WAV from flash through the speaker over I²S,
      no network involved.** This is your deliverable for the day and the
      firmware track's dependency. **Prioritise it over everything else,
      including the enclosure.**
      - Wiring: module VCC → 4.8 V rail (not 3.3 V — more volume, and the module
        is rated 3.3–5 V). GND → common ground. BCK/LCK/DIN → GPIO **32/33/14**.
      - Test audio: `tools/phrases_pcm16/*.wav` — already 16-bit PCM at 24 kHz.
- [ ] 🔴 **Resolve the SCK question, in this order** (§4). It's a 20-minute test
      that otherwise becomes a two-day bug:
      1. Try it with **SCK unconnected**. Clean audio? Done — GPIO 0 stays free.
      2. Silence or noise? **Ground SCK** and retry.
      3. Still nothing? **Drive MCLK from GPIO 0** and configure it in I²S setup.
      → Tell firmware which one worked.
- [ ] 🔴 **Which channel is the speaker on?** `ONLY_LEFT` vs `RIGHT_LEFT`.
      → Tell firmware. Both answers go in `hardware/wiring.md`.

### Power

- [ ] Wire the two 2×AA holders **in series** → 4×AA NiMH ≈ 4.8 V → board VIN.
      *(A single 4×AA holder is cleaner if you have one — one part, no series link.)*
- [ ] **1000 µF as close to the board's VIN pin as physically possible** —
      absorbs Wi-Fi transmit spikes.
- [ ] **470 µF at the audio module's VCC** — stops playback dragging the rail
      down and resetting the ESP32 mid-sentence.
- [ ] **0.1 µF ceramic across each**, closer to the chip than the electrolytic,
      legs trimmed short.
- [ ] ⚠️ **Check electrolytic polarity twice before power-on.** Reversed ones
      vent. `01-HARDWARE.md` §9 bite #2.
- [ ] ⚠️ **Verify common ground** between the board and the audio module. Most
      common I²S failure there is — noise, or nothing at all.

### Then

- [ ] Bench step 4 — both buttons on GPIO 13 and 15, `INPUT_PULLUP` to GND,
      debounced, clean single events on serial with no double-fires.
- [ ] Bench step 5 — repeat steps 2–4 on battery instead of USB. Log the voltage.
- [ ] **Two diffused white LEDs either side of the lens** — not one, a single
      source shadows raised label text. Scrap of white PETG makes a diffuser.
      D18 promoted this from polish to primary accuracy mechanism.
- [ ] **Volume test.** Full-volume speech, phone SPL meter at 0.5 m, want
      **≥ 75 dB**. Then take it into the noisiest room you can find. If it
      disappoints, a 4 Ω speaker is a $5 fix — but only if you find out today.

### Record as you go → `hardware/measurements.md`

Idle current · peak current on Wi-Fi TX · current during playback · pack voltage
fresh · SPL at 0.5 m. All eight rows are currently blank and every one is a
number that goes straight into the writeup.

---

## Part 4 — Firmware lane

**Owner: computer engineer + 1 SWE pairing.** The cloud track is finished, so
that SWE is free — the plan always intended this pairing.

You develop against the **mock server**, not OpenRouter. Plain HTTP, deliberately.

```
python tools/wav_to_phrases.py --emit-header    # generates phrases.h
python tools/mock_server.py                     # banner prints the LAN IP
```

Put that LAN IP in `MOCK_BASE_URL` — **not `127.0.0.1`, the ESP32 can't reach
it.** Board and laptop must be on the same network; this is a classic day-1
time sink.

- [ ] **Project skeleton.** `platformio.ini` is already correct and pins
      `espressif32@6.5.0` (Arduino-ESP32 core 2.0.14) — the legacy `i2s_config_t`
      API in the brief matches that core. Don't unpin it (D9).
- [ ] **`camera.cpp`** — capture a JPEG to PSRAM, print its size over serial.
      `fb_location = CAMERA_FB_IN_PSRAM`, `fb_count = 1`, `FRAMESIZE_SVGA`,
      quality 12.
- [ ] **Log `ESP.getFreeHeap()` before and after every phase, from the first
      commit.** §6.1 — this is where the crashes live, and retrofitting the
      logging after you have a crash wastes a day.
- [ ] **`net.cpp` — the streaming base64 uploader.** The `B64Stream` class is
      already written out in `02-SOFTWARE.md` §6.2; port it as-is. Content-Length
      is deterministic: `prefixLen + b64.encodedLength() + suffixLen`. Peak extra
      RAM about a kilobyte. **Do not build the base64 as an Arduino `String`** —
      that's 200 KB+ of peak allocation and it fails during the demo, not on the
      bench.
- [ ] **`audio.cpp`** — you can write it now, but you can't test it until the
      hardware lane answers MCLK and channel. **Port `tts()` from
      `tools/reference_pipeline.py`** — it's the only working implementation of
      the real audio path and it's deliberately written the way `audio.cpp` has
      to be: SSE line framing, per-delta base64 decode, one-byte sample carry.
      Note each SSE delta is independently padded base64, so it decodes
      standalone — the only carry you need is the 16-bit sample alignment.
- [ ] **Test the failure paths early** — the mock injects them on demand:
      ```
      curl -X POST http://localhost:8080/mock/scenario -d scenario=notext
      #   ok | notext | uncertain | http500 | timeout | garbage | empty
      ```

### Acceptance for today

> **Press reset → the board takes a photo → canned text appears over serial.**

That's the plan's *day 3* integration checkpoint. Hit it today and you're two
days ahead of a schedule that has no slack in it.

---

## Part 5 — Cloud lane

**Owner: 1 SWE.** Your track is done; the highest-value thing you can do today
is the item with the longest lead time and the one that needs a human with a
camera.

- [ ] 🔴 **Shoot the eval photos.** Zero of them exist —
      `tools/eval/images/` is empty and `expected.json` has three "FILL ME IN"
      stubs. **Cut the target from 20 to 10** and weight all ten to the case
      that actually hurts someone: doses, expiry dates, dollar amounts, bus
      numbers.
      - **Real objects, not printouts**: medicine bottles (curved, small print),
        a glossy laminated menu, a low-contrast utility bill, handwriting, a bus
        timetable.
      - **Shoot them through the actual device** once the camera works — the
        mock saves every upload to `tools/captures/`, which is the easiest way
        to build the set with the real lens and the real distance.
      - Fill in the `numbers` and `must_contain` ground truth as you go.
- [ ] **Run the eval and settle D16.** `python tools/eval/score.py`. The model
      swap is worth 1.4 s of latency and it's been sitting behind ten
      photographs. Current headroom for the entire device side is **0.95 s**;
      the swap takes it to **2.86 s**.
- [ ] **Add a frontier vision model to `compare_models.py` and re-run the D17
      hard image.** All four current candidates are budget models
      (`flash` / `flash-lite`), chosen when cost mattered. It doesn't any more.
      D18 concluded "no model swap fixes accuracy" from a set that never
      included a top-tier model. For the one failure mode that could actually
      hurt someone, that gap is worth closing — four lines and one re-run.
- [ ] **Once the voice decision lands**, generate the remaining 7 phrases,
      including the new D17 one: *"I am not certain of this. Try more light, or
      move closer."*
      Always `--out-dir` somewhere scratch first and listen before committing.

---

## End of day — what good looks like

| | |
|---|---|
| ✅ **Minimum** | Camera streams in a browser · pin map confirmed and written down · lens refocused · all four decisions made and logged |
| ✅ **Good** | The above, plus a WAV playing out of the speaker over I²S, and the board POSTing a real photo to the mock |
| ✅ **Ahead** | Both of those, on battery power, with the day-3 checkpoint hit and ten eval photos shot |

**Standup tomorrow, 15 minutes:** what shipped, what's blocking, has the eval
score moved.

---

## Reference — commands

```bash
# once, before anything else
python tools/wav_to_phrases.py --emit-header

# the thing firmware talks to, all day
python tools/mock_server.py

# check the mock still works after any change to it
python tools/test_mock.py                       # 21 assertions

# ground truth — if the device disagrees with this, the device is wrong
python tools/reference_pipeline.py photo.jpg --base-url http://127.0.0.1:8080/api/v1
python tools/reference_pipeline.py photo.jpg --mode read --text-only   # free, no TTS

# failure injection
curl -X POST http://localhost:8080/mock/scenario -d scenario=notext
curl http://localhost:8080/mock/status
```

**Companions:** `00-TEAM-PLAN.md` · `01-HARDWARE.md` · `02-SOFTWARE.md` ·
`docs/decisions.md` · `docs/measurements.md`
