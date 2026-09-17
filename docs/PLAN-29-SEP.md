# Talking Reader — the 18 days to 29 Sep

## Context

The showcase moved from 15 Sep to **29 Sep**. Today is 11 Sep, so 18 days instead of 4.

That changes the nature of the work. On 8 Sep the goal was "get one real photo read aloud before we run out of time". The device half of that is now **done and measured**: camera captures a QXGA frame where small print is legible (554 ms), POSTs it in the contracted shape, receives audio, parses the header, and plays every sample (151566 of 151566, no leak). What is missing is not device capability — it is a server that does a real vision call, a device that behaves well when things go wrong, and any evidence that a person who cannot see can actually use it.

Priorities, as set by the team:

1. **Mode A (Read) done genuinely well** — top
2. **Ultrasonic aiming assist** — top
3. **Error handling** — top
4. Summarise / Describe — a stretch, only if the above are solid

Demo-day architecture: Kristian's server, **run from his laptop and from Sam's laptop as a second copy**, on Sam's phone hotspot.

**Ownership:** Sam owns all firmware (the other SWE's job ended with `camera.cpp`). Kristian owns the server. Mechatronics owns the LEDs, the enclosure and the sensor mounting. So the firmware phases below are sequential, not parallel — size them accordingly.

**Battery is deliberately deferred.** It stays on USB for now; the priority is exercising the live path. Revisit around 22 Sep if Phases 0–2 are solid — it fills four judge-facing measurement rows and makes this a real handheld rather than a tethered demo, but it also introduces brownout-on-Wi-Fi-TX as a new failure mode, and that is not a thing to be debugging in the last week.

---

## Where things actually stand

### Works, proven on hardware

Camera capture at QXGA · button events · Wi-Fi · I²S audio out · WAV/PCM parsing and playback · HTTP status → spoken phrase mapping · the mock server in both API shapes · failure injection.

### Broken — found by audit on 11 Sep, not yet fixed

| | Impact |
|---|---|
| 🔴 **`client.cpp`'s body read loop has no deadline, no `connected()` check and no yield.** A server that stalls mid-body spins core 1 forever, and the CPU1 idle watchdog is not enabled. | **Permanent silent brick, no recovery.** Worst defect in the firmware. See 1.0. |
| **Repeat (B-long) always says "Nothing found. Try moving closer."** `g_have_last` is only set in the `#else` branch, so on the live `USE_LOCAL_SERVER` path it is never true. | Actively **false information** after a successful read, in a shipping feature. And `PH_NO_TEXT` is one of the three phrases that *do* exist, so the lie is reliably audible. |
| **Long-press gets no shutter and no announcement.** `audio_write()` aborts on `buttons_any_down()`, and long events fire while the finger is still down. | Summarise and Repeat feel dead on press. |
| **`earcon_error()` is truncated** 180 ms → 100 ms by the `EARCON_MAX` clamp. | It is the *only* audible failure cue today, since `PH_ERROR` is unrecorded. |
| **`earcon_tick()` exists and is never called.** | The designed silence-filler is absent. |
| **No watchdog, no state timeout.** `g_state` is write-only; `ST_ERROR` never assigned. | A hang is silent and unrecoverable. Worst case ~60 s of silence (30 s Wi-Fi + 30 s server timeout). |
| **`audio_drain()` is a blind ~190 ms `delay()`**, twice per press. | ~380 ms of dead time per interaction, for nothing. |
| **`speech.cpp` is unreachable** but still links, dragging in the TLS stack. | Dead weight and a misleading code path. |
| **`buttons.h` comment names GPIO 13/15** — the stale WROVER pins. | Next reader wires the camera pins. |

### Missing entirely

- **7 of 10 phrases** — `reading`, `describing`, `connecting`, `batt_low`, `error`, `repeating`, `uncertain`. The device is silent on most failure paths, violating the project's own stated rule.
- **Ultrasonic** — wired (TRIG 38, ECHO 14, 3.3 V, pins verified clear), nothing reads it.
- **Battery monitoring** — no ADC, no sense line, no threshold. `PH_BATT_LOW` has zero call sites. *Hardware side isn't wired either.*
- **The two LEDs** — GPIO 39 free, not fitted.
- **Eval set** — 0 images, 3 stub entries. `score.py` currently exits 0 having scored nothing.
- **Measurements** — all 8 hardware rows and 6 software rows blank.
- **User testing** — nobody booked.

### The one thing blocking a real reading

**Kristian's server is not reachable.** Scanned the hotspot: only `172.20.10.4:5148` answers, and that is our own mock. `172.20.10.7` (his laptop) has none of 5000/5001/5148/7148/8080 open. Classic `localhost` binding — works in his browser, invisible to the network.

---

## Phase 0 — first real reading (today / tomorrow)

Nothing in the rest of this plan is needed for this. Three items, all Kristian's:

1. `dotnet run --urls http://0.0.0.0:5148` — verify with `netstat -an | findstr 5148` showing `0.0.0.0`, not `127.0.0.1`. Allow through Windows Firewall on **Private**; if Avast is installed it overrides the Windows rules.
2. Use the real prompt — `PROMPTS["read"]` in `tools/reference_pipeline.py:54`. The `UNCLEAR` / `[?]` tokens are load-bearing: `main.cpp` greps for them.
3. Return **24 kHz mono**. Wrong rate plays chipmunked; **stereo plays as noise with no error anywhere**. WAV or bare PCM both fine — `reader.cpp` handles either, so "raw PCM not WAV" can be dropped from his list.

Verify before involving the board: `POST` a known-good JPEG from `tools/captures/` with `curl --data-binary @file -H "Content-Type: image/jpeg"`, and check the returned audio is 24 kHz mono.

**Definition of done: point the device at a medicine label, press A, hear that label's text.**

---

## Phase 1 — the device must never be silent

The project's own non-negotiable, currently violated on most paths. Ordered by **probability × severity to a user who cannot see the device**, not by effort.

### 1.0 🔴 `client.cpp` can brick the device permanently — fix first

Found on verification; it was not in the original audit and it is the most dangerous defect in the firmware.

```cpp
while (bytes_read < wav.size()) {
    size_t available = stream->available();
    if (available > 0) { … }
}
```

No deadline, no `stream->connected()` check, no yield. If the server sends a `Content-Length` and then stalls — laptop sleeps, hotspot drops, Kokoro dies mid-write — `available()` returns 0 forever and this spins on core 1. **The CPU1 idle-task watchdog is not enabled in the Arduino SDK config, so nothing recovers it.** The device goes silent and stays silent until the battery is pulled — indistinguishable, to a blind user, from a flat battery.

`http.POST()` itself *is* bounded (HTTPClient defaults to 5 s). The unbounded part is the hand-rolled body loop.

Fix is six lines: a `millis()` deadline, a `connected()` check, a `delay(2)` yield, and a new `-2` status for "reached the server, body incomplete". **This is Kristian's file** (`client.h`: *"keep Claude off of this class lmao"*) — hand it over as a diff with the failure explained, rather than landing it unilaterally.

Related, and worth telling him at the same time: **`SERVER_TIMEOUT_MS` is never actually used in the shipping build.** It appears only in `restest.cpp`, which `build_src_filter` excludes. `SERVER_CONTRACT.md` currently documents a timeout the firmware does not enforce.

### 1.0b Two config values that make everything worse

- **`WIFI_SSID_2` is still the untouched template** (`"hotspot-two"` / `"password"`). So the 30 s connect hole isn't a worst case — it's the *normal* case whenever the hotspot is off. Fill it or delete the second slot.
- **Wi-Fi connects lazily on first press**, so that hole is reachable at the worst possible moment. Connect in `setup()` with `setAutoReconnect(true)`.

### 1.1 Generate the 7 missing phrases

The voice-cloning decision (Route A vs B) is **obsolete** — both routes assumed the cloud voice came from OpenRouter, and the server now synthesises with **Kokoro**. Generate all 10 phrases with Kokoro in the server's own voice: same engine, same voice, no cloning, no HuggingFace gate.

- Ask Kristian which Kokoro voice he has configured.
- Repoint `tools/make_phrases.py` at Kokoro. The pocket-tts coupling is ~6 lines in `main()`; everything downstream (`to_numpy`, `write_wav_f32_as_int16`, trim, the `phrases.h` handoff) is engine-agnostic. Drop the mimi-specific `--max-tokens` ceiling.
- Generate to a scratch `--out-dir` first and **listen** before writing to `Messages/`.
- `python tools/wav_to_phrases.py --emit-header`.

Exact texts are already in `make_phrases.py`. Add one new phrase for the aiming assist if the design calls for it.

**The lever that makes the rule enforceable:** `phrase_play()` currently returns `true` for a phrase that doesn't exist, so nothing in the codebase can tell "spoke" from "was silent". Change it to return `PhraseResult { PHR_SPOKEN, PHR_EARCON, PHR_STOPPED, PHR_SILENT }` and give **every phrase id a distinct synthesised earcon fallback**. The fallbacks must differ from each other — a user who hears the same buzz for "no internet" and "no text" has learned nothing, and learning the device is their only feedback channel. When the recordings land they take priority; the earcons stay as the permanent backstop, because a corrupted phrase bank must not mute the device either.

Add `uint32_t phrase_silent_count()`, printed per press, so the never-silent rule is **measured rather than asserted**.

Two new phrases, both of which remove a *false statement* (marginal cost zero since the Kokoro batch is running anyway):
- **`PH_NO_SERVER`** — "I cannot reach the reader. Check the laptop." Used when `status <= 0` *and* Wi-Fi is connected. Fixes the hazard where the device blames the network for a sleeping laptop.
- **`PH_NOTHING_TO_REPEAT`** — "I have not read anything yet."

Also replace the fixed 2400-sample earcon buffer with a streaming generator. That removes the silent clamp (which cost `earcon_error()` 44% of its length for weeks), removes 4.8 KB of `.bss`, and enables multi-blip fallback sequences.

### 1.2 Fix Repeat properly — the cache *is* the playback buffer

Decode the server's reply straight into a PSRAM cache, then play from the cache. One buffer, no extra copy, and — the property that matters — **every successful read exercises the Repeat path**, so it cannot silently rot the way it has all week.

- `REPEAT_CACHE_SECONDS 30` = 1.44 MB, allocated **once at boot** and never freed. A per-press allocation is a failure path that first appears at press #7 in front of judges.
- **Strip the WAV header at cache time**, storing only converted int16. Then `repeat_have() == true` means "this is 16-bit mono at 24 kHz and it already played once" — an invariant `handle_repeat()` can lean on with no network and no server.
- **Invalidate at the *start* of each press, not the end.** A user reads bottle A, moves to bottle B, presses, that read fails — with a stale cache, Repeat then speaks **bottle A's dose while pointed at bottle B**. That's the same class of error as today's false "Nothing found", but quieter and more dangerous because it sounds like a successful read. Not a close call.
- **The demo beat: pull the hotspot, press B long, hear the reading.** Rehearse it.

### 1.3 Fix the long-press feedback bug

`audio_write()`'s predicate conflates two questions: "does the user want to interrupt this?" (right during speech) and "is a button physically down?" (wrong the moment a long press fires). Add `audio_hold_stop_until_release()` — suspends the stop rule until both buttons come up, then re-arms. Six lines, one named concept, no change to the interrupt guarantee.

### 1.4 A silence policy, and why it needs a task

> **From the moment a button event is classified until the device returns to IDLE, the speaker is never quiet for longer than `SILENCE_MAX_MS` (1200 ms).**

One `wait_with_ticks()` pump, driving the already-written `earcon_tick()`. Free bonus: a press during the wait **cancels** the read — that's not the forbidden queued press (nothing is queued, no second call), it turns the worst-case wait from the device's problem into the user's choice.

**This needs the network POST on a FreeRTOS task, and the argument is decisive:** an in-line tick can cover the Wi-Fi wait and the body download — both our code — but it cannot cover `HTTPClient::POST()`, and the server's entire vision + Kokoro time is blocked inside that one call. Measured: 6837 ms silent on a *working* server. There's no hook, no yield, no callback. An in-line tick isn't a cheaper version of the fix; it's a version that leaves the largest hole open.

Concurrency surface kept deliberately tiny: exactly one job in flight; the worker produces bytes and **never touches I²S**, so the foreground owns the DAC outright and no mutex exists anywhere; the worker owns the JPEG and calls `camera_release()` itself so an abandoned job can't have its buffer freed underneath it. On timeout, do **not** `vTaskDelete` — killing a task blocked in lwIP leaks the socket. Refuse new presses until it drains; the supervisor is the backstop.

### 1.5 State supervisor and watchdog

`g_state` is currently write-only and `ST_ERROR` is never assigned. Give every state a budget derived from measurement (capture 4 s against 554 ms measured; wait-server 25 s against 6837 ms measured). Soft trip → speak the error and return to IDLE. Hard trip → **make a sound, then reset**: a silent reboot is indistinguishable from a flat battery.

**Arm the task watchdog on the supervisor task only** — not on `loopTask`, which legitimately blocks for 7–20 s against a 5 s timeout. Layering: worker timeout (20 s) → state budget (25 s) → supervisor hard reset (+5 s) → TWDT (only if the supervisor itself dies). Four bounds, each looser than the last, so the most graceful one normally fires.

### 1.6 Keep `speech.cpp`, gate it behind `#if !USE_LOCAL_SERVER`

**The reason to delete it was false.** Measured from the link map: 9,670 bytes of `.text`, 4 bytes `.bss`, **zero runtime heap**. It does not pull in TLS — `WiFiClientSecure` comes from `client.cpp`'s `http.begin(String)` overload, and mbedTLS from `wpa_supplicant` (i.e. from using Wi-Fi at all). It would all be in the image with zero application code.

Keep it because D25 is explicitly *conditional* on the server staying on the LAN, the mock fallback "must keep working", and the file encodes three bugs that cost real days (D14's SSE shape, D23's whitespace-tolerant `"data":` scan, the one-byte sample carry). The actual dead code is `speak_result()` and `g_last_text[1024]` **in `main.cpp`** — gate those in the same change.

### 1.7 Failure-injection test pass

Run the mock on `--port 5148` so it stands in for the real server with **no reflash**. Two gaps in the rig to close first:

- **The `uncertain` scenario is a no-op on `/api/tts/fromimage`** — it falls through to a normal 200. So D17, the project's most dangerous failure mode, is **completely untested on the shipping path.**
- **Add a `stall` scenario** — a `Content-Length` that's never satisfied. It's the only reproducer for 1.0, the defect that currently bricks the device.

Pass criterion per press: a sound within 1.2 s, a sound at least every 1.2 s, a correct and *distinguishable* final sound, and a return to IDLE. Run the whole matrix twice — once with 3 phrases (exercises the earcon fallbacks), once with all 12.

**The 50-press soak.** Define failure precisely and write the definition into the table: *(a) silent longer than `SILENCE_MAX_MS` at any point, (b) said something false, (c) didn't return to IDLE within budget.* (a) and (c) are measured by the device itself via a per-press summary line; only (b) needs a human. Three numbers for the writeup: failure rate, worst silence observed, heap at press 1 vs press 50.

---

## Phase 2 — ultrasonic aiming assist

The one thing a blind user genuinely cannot do is judge whether the label is at the 15–25 cm the lens is focused for. D18 made image quality the accuracy lever; D28 narrowed it to **light and distance**. Distance is the last variable the user controls and the only one they cannot perceive.

### The governing rule

**It advises, never vetoes.** Bounded by `AIM_MAX_MS 1500`; every exit — sensor absent, stuck, unstable, timed out, interrupted — falls through and takes the photo anyway. An optional sensor must never be able to stop the device working.

### The interaction: non-verbal, inside the press

Not continuous. A device that beeps while held masks the hearing a blind user navigates by, marks them out in public, and burns ~15 mA all day for feedback nobody asked for.

- **Pitch carries direction** — low tick (~800 Hz) = come closer, high (~2000 Hz) = back off.
- **Repetition rate carries error, and faster = warmer** (2 Hz far → 8 Hz nearly there). Deliberately the inverse of a parking sensor: here the good state is the destination, and it resolves into silence plus the shutter.
- **Speech is spent once, and not in the loop.** When the server says "nothing readable" *and* we measured the aim as out of band, `PH_NO_TEXT` is replaced by a direction-specific hint: *"Nothing readable. Try holding it closer, about a hand-span away."* **"A hand-span" is the key phrase** — it's a non-visual reference a user can actually apply; "twenty centimetres" is not.

### Fast-accept — the feature is free when aiming is already good

One ping. If it lands inside the inner band (17–23 cm), shoot immediately — **~2 ms, no median, no ticks**. A false "good" costs a photo we were taking anyway; a false "bad" costs the user 1.5 s. Given D28 already spent 640 ms of budget on QXGA, this matters.

### Where it runs

New `ST_AIMING`, **inline in `handle()`**, placed *after* `wifi_connect()` (a cold connect can block 15 s, and a distance measured 15 s before the shutter is a lie) and immediately before `camera_capture()`.

**Not a FreeRTOS task** — three concrete blockers, worth recording so nobody rediscovers them: `g_earcon[2400]` in `phrase.cpp` is a shared static buffer; the I²S port has no mutex; and `pulseIn()` reads the per-core cycle counter, so an unpinned task would return intermittent garbage under load.

### `earcon_captured()` — do not cut this

The shutter earcon fires at t=0 as a *press* acknowledgement. This feature inserts up to 1.5 s between that and the actual exposure, so a user hears the shutter, assumes it's done, and **lowers the device before the frame is taken.** This feature creates that bug, so it must close it: a distinct double-blip right after `camera_capture()` returns. Four lines.

### Driver

`pulseIn()` with `US_ECHO_TIMEOUT_US 12000` — worst-case block is exactly the timeout (verified against the core's `wiring_pulse.c`), which is **shorter than one 21 ms I²S DMA chunk**. Behind one function so RMT is a local swap later.

- **`pinMode(ECHO, INPUT_PULLDOWN)`** — this is what makes sensor absence *detectable*. A floating pin returns plausible garbage; a pulled-down pin times out honestly.
- Rolling median of 5, samples expire after 400 ms, need ≥3 fresh.
- **`spread > 10 cm` is the curved-bottle detector.** Specular loss off a glossy curved surface makes the beam miss and you read the bench behind it. On unstable readings the device **says nothing at all** — guiding someone wrong is worse than not guiding them, because they can't tell the difference.
- Boot probe of 6 pings; zero echoes ⇒ absent for the session, every press behaves exactly as today, and **nothing is spoken about it** (compare D24 — don't announce a broken component the user can't see, can't fix, and doesn't need).

### Bench measurements needed before committing to numbers

1. `US_OFFSET_CM` — read a taped 20 cm; an enclosure lip is a permanent offset
2. **Does it work at 3.3 V at all**, and to what range (sweep 5→100 cm)
3. Jitter at 20 cm **with Wi-Fi busy** — `pulseIn` busy-waits, so an ISR inflates readings. If σ > 1.5 cm, move to RMT
4. Can `US_REARM_MS` drop from 60 to 30
5. **Tick audibility at 800 Hz** on the assembled speaker — a small 8 Ω driver in plastic rolls off hard below ~700 Hz. Check `earcon_error()`'s 320 Hz at the same time; it may already be inaudible

If the 3.3 V module is flaky, **HC-SR04P / RCWL-1601 are pin-compatible and rated 3–5.5 V.** Order one now so it's a swap, not a debugging session.

### Kill switch

`US_ENABLE 0` restores today's behaviour byte-for-byte. The one thing that cannot be resolved at a bench is whether real users find the ticks helpful or irritating — that needs Phase 5.

### Two electrical traps (for mechatronics)

1. **Do not drive two LEDs directly from GPIO 39.** 2 × 20 mA exceeds the S3's per-pad maximum. Use a small N-MOSFET or NPN with the LEDs on the 5 V rail. Direct drive works on the bench and browns out under battery — presenting as a random reboot.
2. **If using LEDC for LED brightness, do not use channel 0 or 1.** `camera.cpp` claims `LEDC_CHANNEL_0`/`LEDC_TIMER_0` for XCLK, and Arduino pairs channels onto timers. Collide and *the camera* breaks, baffling everyone. Use channel 4.

### Bonus: the LEDs and the aim window pay for each other

Turn the LEDs on at the start of the aim window and auto-exposure gets up to 1.5 s to converge instead of the ~150 ms `CAM_DISCARD_FRAMES` currently buys.

### Files

New `distance.{h,cpp}` (pins, pinging, filtering — knows nothing about sound), `aim.{h,cpp}` (the window, tick pattern, verdict), `lights.{h,cpp}`, and an `aimtest.cpp` bench env mirroring `restest`. Earcons stay in `phrase.cpp` so there's one vocabulary and one buffer. Add `bool phrase_available(PhraseId)` so the hint phrases can fall back to the recorded `PH_NO_TEXT` until they're generated.

**`lights.cpp` as its own file is a team call, not an architectural one** — `camera.cpp` belongs to the other SWE and was just confirmed working on silicon; don't create a merge conflict on it.

---

## Phase 3 — accuracy and evidence

- **Shoot the eval photos.** 10 minimum, weighted to what hurts: doses, expiry dates, dollar amounts. Real objects — curved bottles, a glossy menu, a low-contrast bill, handwriting. The device saves every upload to `tools/captures/`, so shoot them through the real lens at the real distance. There are already 18 captures there to start from.
- Fill `expected.json` — `numbers`, `must_contain`, `expected_text`. Note `score.py` currently exits 0 with zero coverage, which is worse than failing.
- **Fit the two LEDs** (GPIO 39, diffused, one either side). Every frame so far is underexposed, and `CAM_AE_LEVEL -1` deliberately makes that worse to buy back motion blur. Light removes that trade.
- **Re-run the eval** and settle D16 (the model swap worth 1.4 s and 37%).
- **Fill `hardware/measurements.md` and `docs/measurements.md`** — 14 blank rows, all judge-facing numbers.

---

## Phase 4 — demo-day resilience

- **Kristian publishes self-contained** (`dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true`) and it runs on Sam's laptop. Removes his laptop, his .NET install and his presence as single points of failure. ~70–150 MB — move it on a USB stick, not over the hotspot.
- Keep `tools/mock_server.py --port 5148` working as the last-resort fallback.
- ⚠️ **The hotspot IP is a DHCP lease.** It has already changed once mid-project. Check `ipconfig` before every session; a stale `SERVER_BASE_URL` looks exactly like a dead server and costs a reflash.
- Build the **spare unit**. The plan's own rule: "the demo unit is not the test unit."
- **Rehearse the demo three times.**

---

## Phase 5 — user testing

Booked in week 1 for week 3 was the plan; nobody is booked. The stated pass/fail is *"a stranger uses it unaided."* The team has said they will be the testers — that is the one assumption the device cannot check, in the same way the user cannot check the numbers it reads them. Even one or two people with vision loss changes what this is worth. A blindfold session with someone not on the team is a poor substitute that still exposes the aiming and button problems in minutes.

---

## Stretch — Summarise and Describe

Cheap in firmware: the prompts are already written verbatim in `tools/reference_pipeline.py`, the state machine does not branch on mode, and `PH_DESCRIBING` is already in the phrase bank. **But the device currently has no way to tell the server which mode** — all three paths POST the same URL. Needs either distinct paths or a header. Do not start this until Phases 0–2 are solid.

---

## Set a freeze date

The original plan put a feature freeze at day 22 of 28 and said to "write the date on the wall". With 29 Sep as the showcase, **freeze on 26 Sep** and leave the last three days for fixes and rehearsal. Also resolve the date inconsistency: `CLAUDE.md` and `docs/TODAY.md` disagreed about 14 vs 15 Sep; they should now both say 29 Sep.

---

## Verification

| Phase | How you know it is done |
|---|---|
| 0 | Point at a real medicine label, press A, hear that label's own text. |
| 1 | Every mock scenario produces a correct spoken response. 50 consecutive presses with a measured failure rate. No silent path. |
| 2 | A blindfolded person finds reading distance using only the sound the device makes. |
| 3 | `python tools/eval/score.py` scores ≥ 10 real images with a numbers score, and the score is recorded. |
| 4 | The server runs from Sam's laptop with Kristian absent from the room. |
| 5 | Someone who is not on the team completes a read unaided. |
