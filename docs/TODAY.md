# Today — 18 Sep 2026 · Sam, solo

**Showcase is 29 Sep 2026 -- 11 days out.** `CLAUDE.md` said 15 Sep, which was
three days ago; the move is recorded in `docs/PLAN-29-SEP.md` on the `Refactor`
branch, which never reached `main`. `CLAUDE.md` is corrected. That plan also
sets the priorities -- Mode A done well, ultrasonic aiming assist, error
handling -- and today's work lands squarely on the first and third.

The 8 Sep session is archived in `docs/sessions/2026-09-08.md`. It was ten days
stale and still said the server refused the connection — which has not been
true since about 17 Sep.

---

## The presenting problem

> "The ESP32 is currently connected, it seems to be broken, neither button does
> anything."

**The board was never broken.** It was running `smoke` — the liveness blinker
from an earlier bisect, which never configures either button pin. Buttons were
physically incapable of doing anything.

It was running `smoke` because **`pio run -e freenove_s3` had been failing**, so
the upload step never ran and the previous image stayed put. A failed build
leaves a board that boots, blinks and looks alive.

### The failure signature, for next time

```
.pio/build/freenove_s3/          <- no firmware.bin, no firmware.elf
.pio/build/freenove_s3/src/
    main.cpp.d    present        <- the compiler was invoked
    main.cpp.o    ABSENT         <- and it failed
    speech.cpp.d  present
    speech.cpp.o  ABSENT
    audio/buttons/camera/phrase/reader/client .o   all present
```

Two `.o` files missing out of eight, and the two that were missing were exactly
the two files referencing macros that `config.h` no longer defined.

### Root cause

`config.h` was rewritten on 17 Sep for the server path and dropped nine macros
the sources still used: `USE_MOCK_SERVER`, `MOCK_BASE_URL`, `OR_BASE_URL`,
`OR_API_KEY`, `TTS_MODEL`, `TTS_VOICE`, `TTS_AUDIO_FORMAT`, `VISION_MODEL`,
`VISION_MAX_TOK`. `config.h` is gitignored, so there was no diff to notice.

The worse half was **`USE_LOCAL_SERVER`, which was defined nowhere in the repo
at all** — not in `config.h`, not in the template, not in `platformio.ini`.
Undefined in `#if` is silently `0`, so the adopted-server path (`main.cpp`
151-204) was compiled *out* and the unwritten `vision.cpp` branch compiled *in*.
Had the file compiled, a green build would have spoken canned stub text through
a TLS path with no key. See **D30**.

---

## What changed

One backend, no branches (**D29**). Press → `camera_capture()` →
`send_jpeg()` → `read_aloud()`.

**Deleted:** `speech.cpp`, `speech.h`, `tools/mock_server.py`,
`tools/test_mock.py`, `tools/mock_tts_sample.wav`, and the `lib_deps` on
ArduinoJson — `speech.cpp` was its only user, so the device no longer builds or
parses JSON at all.

**`main.cpp`** — the `#if USE_LOCAL_SERVER` / `#else` pair and the whole
two-leg branch are gone, along with `speak_result()`, `text_is_uncertain()` and
`g_last_text` (the device never sees a transcript, so they were dead). The
banner now prints the real server URL, which also fixes the wrong-backend
banner logged on 8 Sep.

**Button B long now works, offline.** The reply is downloaded *straight into* a
1.44 MB PSRAM buffer allocated once at boot, so a successful read is already
cached — Repeat costs no network, no capture and no second API call, and needs
no copy. This is what `config.h REPEAT_CACHE_SECONDS` was always budgeting for.

**`client.cpp`** (Kristian's, amended with permission) — three fixes:

1. `http.setTimeout(SERVER_TIMEOUT_MS)`. It was never applied, so HTTPClient's
   **5 s default** was in force. A real press later measured **16.6 s**, so this
   alone would have failed long readings even after the build was fixed — and
   failed them as "no internet".
2. The reply lands in a caller-owned **PSRAM** buffer. It used to come back in
   a `std::vector`, which allocates in internal heap — and a real press later
   returned **608,582 B** against a largest contiguous block of **237,556 B**.
   Long labels would have thrown `bad_alloc`, also reported as "no internet".
3. The download loop is bounded — deadline, `connected()` check, and a
   `delay(1)` so it yields. It had none of those: an early disconnect or an
   over-declared `Content-Length` was an infinite tight spin that starves the
   idle task and trips the watchdog.

**Two latent hangs fixed.** `buttons_begin()` left `pressed_at` at 0, so a pin
reading LOW at boot fired a **spurious long press ~740 ms in, with nobody
touching the device**; `main.cpp`'s swallow loop then waited forever for a
release, and because `audio.cpp` also polls `buttons_any_down()` inside every
playback chunk, the device went silent at the same time. One stuck pin =
permanently dead device. Now seeded properly, and the swallow loop has a 3 s
ceiling that logs which GPIOs to check.

**`config.example.h` is now generated** by `tools/sync_config_template.py`, with
credentials scrubbed, and `--check` verifies it. This is the fix that stops
D30 happening again — the template can no longer lag the live file.

---

## Verified today

Build: **SUCCESS**, RAM 17.0%, flash 36.0% (1,133,808 B). Flashed over COM4.

**The server is fine and was never the problem.** From this laptop, no device
involved: `GET /api/tts/fromimage` → 405 (route exists, wants POST), and real
photos POST cleanly.

| Image | Result | Time | Body |
|---|---|---|---|
| ceiling vent, no text | 422 "No text found." | 10.2 s | 14 B |
| printed box, small text | 200 `audio/wav` | 3.9 s | 124 KB = 2.6 s speech |
| dense text | 200 `audio/wav` | 9.9 s | 585 KB = 12.2 s speech |

Format confirmed by header parse: **1 channel, 24000 Hz, 16-bit** — exactly
what `SERVER_CONTRACT.md` §3 demands. The 422s were correct, not a bug: those
photos genuinely have no text in them.

**The Wi-Fi band question is closed, and the laptop was misleading.** This
laptop sees `ICP-WiFi` only as a 5 GHz BSSID (802.11ax, ch 161), which looked
like the 2.4-GHz-only S3 was locked out. The board's own scan says otherwise:

```
[scan] 16 network(s) visible to the 2.4 GHz radio
  ICP-WiFi                 ch1    -63 dBm  wpa2-psk        <-- WIFI_SSID_1
  ICP-WiFi                 ch11   -64 dBm  wpa2-psk        <-- WIFI_SSID_1
  Curtin                   ch11   -76 dBm  WPA2-ENTERPRISE
  eduroam                  ch11   -76 dBm  WPA2-ENTERPRISE
```

Two 2.4 GHz BSSIDs, `wpa2-psk`. The board can join it. `Curtin` and `eduroam`
confirmed WPA2-Enterprise, as `CLAUDE.md` always said. **Never conclude
anything about the band from a laptop scan** — `WIFI_SCAN_AT_BOOT 1` prints
what the *board* can see.

Boot log is clean: psram 8189 KB, the real server URL, **no `*** STUBBED ***`**
(`camera.cpp`'s strong `pipeline_is_stubbed()` was behind the dead macro too),
`[boot] repeat cache: 1440064 B in PSRAM`, i2s up on 42/41/40, and
`[phr ] ready (0.50s)` — which *played*, and that is itself the proof that
neither button pin is stuck LOW, since `audio.cpp` would have aborted playback.

---

## Verified end to end — it reads

Three real presses on real labels, logged. Full numbers in
`docs/measurements.md`.

| | Press 1 | Press 2 | Press 3 |
|---|---|---|---|
| Capture | 79 KB / 558 ms (cold) | 94 KB / 283 ms | 83 KB / 358 ms |
| `send_jpeg` | 3.2 s, 0 B down | 5.3 s, 79 KB down | **16.6 s, 609 KB down** |
| Server | **422** | 200 | 200 |
| Heard | `no_text` phrase | 1.64 s of speech | **12.68 s of speech** |

Both halves work: it reads a label aloud, and the 422 path plays the recorded
"no text" phrase instead of failing silently. `[play] ok` accounts for every
declared sample both times. Heap is flat across the three presses and PSRAM
returns to exactly 5,675,175 B each time — **no leak**.

**Press 3 is the one to notice.** 16.6 s for the round trip and 609 KB of
audio. Both of today's `client.cpp` fixes were load-bearing for it and nothing
else would have revealed that:

- HTTPClient's default timeout is **5 s**. That press would have failed at 5 s
  and told the user "no internet".
- The reply used to be buffered in internal heap, largest contiguous block
  **237 KB**. 609 KB would have thrown `bad_alloc`.

### The new headline problem: 17 seconds of silence

Press-to-first-word was **~5.6 s** on press 2 and **~16.9 s** on press 3,
against a < 6 s target. The device's share is 283–358 ms of capture — the rest
is the server's vision-plus-synthesis, which scales with how much text there is
to read. **And the device says nothing for the whole wait**, because `reading`
is one of the 7 unrecorded phrases. A shutter click followed by 17 seconds of
nothing is indistinguishable from a broken device to someone who cannot see it.

Recording the phrases is now the **highest-value fix in the project**: it needs
no server work and no firmware work, only the Kokoro voice decision (item 3
below). A "reading…" phrase plus a periodic tick would make a 17 s wait
tolerable instead of alarming.

**Still not exercised: button B long.** The PSRAM replay path has never been
run. Hold B for a second and expect `[stm ] repeat NNNNNN bytes from PSRAM`
with no `[wifi]` or `[stm ] send_jpeg` lines in between; then pull the network
and do it again — it must still speak.

## Still open, most urgent first

1. 🔴 **Bring `docs/PLAN-29-SEP.md` onto this branch.** It is the only copy of
   the 29 Sep date and the agreed priorities, and it is stranded on `Refactor`.
   That branch also carries a `reader.cpp`/`reader.h` rework and deletes
   `pipeline_stub.cpp` -- work that is not in this branch and should either be
   merged or consciously dropped.
2. 🔴 **7 of 10 phrases are unrecorded**, so several failure paths are
   genuinely silent — which breaks the one rule in `CLAUDE.md` that is not
   negotiable. Boot prints them:
   `missing: reading describing connecting batt_low error repeating uncertain`.
   The 8 Sep finding still stands: the server synthesises with **Kokoro**, so
   generate all of them in a Kokoro voice. **Action: ask Kristian which voice.**
   Promoted above everything else today: measured waits of up to 16.9 s happen
   in total silence, which is the difference between a device that is slow and
   a device that looks dead. See the section above.
3. 🔴 **Test button B long** — the only path in the firmware never run.
4. 🟡 **The 422 costs 10.2 s of silence** before the device says "I could not
   find any text" — the worst case, and mis-aiming is the commonest user error.
   The server knows the vision result before it synthesises, so it could answer
   immediately. Raised in `SERVER_CONTRACT.md` §8; needs Kristian.
5. 🟡 **`UNCLEAR` / `[?]` (D17) is unverified end to end.** Nothing we sent
   produced one, so we do not know if the safety half of the prompt is live on
   the server. Needs a deliberately degraded label.
6. 🟡 **No offline test path any more.** Deleting the mock was deliberate and
   the cost was accepted (D29), but it means the press path cannot be exercised
   without both the server and the network up. If a demo ever needs a safety
   net, that is the gap.
7. 🟡 **Eval photos: still 0 of 10.** Carried from 31 Aug, 4 Sep and 8 Sep.
8. 🟡 **Lens refocus and the two diffused LEDs.** D28 narrowed D18: the lens is
   focused acceptably and the **LEDs are now the prime suspect** for accuracy,
   because every frame is underexposed and `CAM_AE_LEVEL -1` deliberately makes
   it worse to buy back motion blur. Mechatronics.
9. ⚪ **Turn Avast back on** — carried from 4 Sep and still not done.
10. ⚪ **`hardware/wiring.md` still flags the camera pin map "not confirmed on
    silicon"**, but 8 Sep confirmed it (OV3660 answered on SCCB, frames
    captured). Two documents disagreeing about a pin map is exactly what D27
    exists to prevent.
