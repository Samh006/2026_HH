# Talking Reader

Handheld assistive device: point it at a medicine label, press a button, it
reads the text aloud. ESP32-S3, camera, two buttons, a speaker, no screen.
Users have vision impairment and cannot check what the device tells them —
which is why wrong numbers matter more than anything else here.

**Showcase: 29 Sep 2026.** Moved from 15 Sep. Recorded in
`docs/PLAN-29-SEP.md` on the `Refactor` branch, which also sets the priorities:
Mode A (Read) done genuinely well, ultrasonic aiming assist, and error
handling -- in that order. Summarise and Describe are a stretch.

## Read before doing anything

1. `docs/decisions.md` — D1–D30. The source of truth. Records *why*, including
   the failure signatures of bugs that already cost us a day.
2. `docs/TODAY.md` — most recent working session: plan and what actually
   happened. Older sessions are archived in `docs/sessions/`.
3. `hardware/wiring.md` — the live pin map. **Rewritten 8 Sep for the S3**;
   `01-HARDWARE.md` §2 is the old WROVER map and is wrong for this board.
4. `docs/SERVER_CONTRACT.md` — what the device POSTs and what it needs back.
   **Start here if you are working on the ASP.NET server** and not the firmware.

## Five things that will otherwise cost you hours

1. **The board is an ESP32-S3-WROOM N8R8, not the ESP32-WROVER** that
   `00-TEAM-PLAN.md`, `01-HARDWARE.md` and `02-SOFTWARE.md` describe. Those
   three briefs are the original plan and are **stale on hardware**.
   `01-HARDWARE.md` §2's pin table is actively wrong: GPIO 13/15 are camera
   pins on this board. Current map: I²S **42/41/40**, buttons **47/21**.

2. **`platformio.ini` must stay `memory_type = dio_opi`.** A `qio_*` build
   hangs the second-stage bootloader *before it prints a character* — esptool
   flashes and verifies fine, then silence and no LED, indistinguishable from
   a dead board. The tell is a ROM log reaching `entry 0x...` and stopping.

3. **Debug on the CH340/UART port, not the OTG port.** The ROM boot log only
   appears on UART0, and it is the only place a boot failure is visible.
   USB-Serial-JTAG stays enumerated whether or not the app boots, so a live
   COM port proves nothing.

4. **`firmware/src/config.h` and `firmware/src/phrases.h` are gitignored.**
   Per-machine. Copy `firmware/include/config.example.h` → `config.h` and fill
   in Wi-Fi and the server machine's LAN IP — there is no API key on the device
   any more (D25). Generate phrases with
   `python tools/wav_to_phrases.py --emit-header`.
   **`config.example.h` is generated** — after changing `config.h`, run
   `python tools/sync_config_template.py`. A macro that exists only in the
   gitignored file breaks every other laptop with no diff to show for it; that
   is D30, and it cost a session.

5. **Campus Wi-Fi ("Curtin") is WPA2-Enterprise and the ESP32 cannot join it.**
   Our code only does pre-shared keys. Confirmed by the board's own scan on
   18 Sep: `Curtin` and `eduroam` both report `WPA2-ENTERPRISE`.
   **`ICP-WiFi` works** — it is `wpa2-psk` and it *is* on 2.4 GHz (two BSSIDs,
   ch 1 and ch 11, −63 dBm), so the S3's 2.4-GHz-only radio can join it. A
   laptop scan is not evidence here: this laptop sees only the 5 GHz BSSID and
   that looked, wrongly, like the board was locked out. `WIFI_SCAN_AT_BOOT 1`
   prints what the *board* can see. The server must be on the same network.

## Commands

```bash
cd firmware
python -m platformio run -e freenove_s3 -t upload   # build + flash
python -m platformio device monitor -b 115200       # watch (UART port)
python -m platformio run -e smoke -t upload         # minimal liveness test

python tools/sync_config_template.py --check         # is the template in step?
```

## The backend — there is only one

The device POSTs a raw JPEG to `/api/tts/fromimage` on Kristian's ASP.NET
server and gets finished audio back. One round trip: the server does the
vision call **and** the text-to-speech, so the device never does TLS, never
base64-encodes anything, never parses SSE, and **never holds an API key**.

- Address: `SERVER_BASE_URL` in `config.h`. It is a DHCP lease — re-check it
  with `ipconfig` on the **server's** machine after any network change. A
  stale address looks exactly like a dead server.
- ASP.NET binds to localhost by default, which the ESP32 cannot reach. It must
  be started with `dotnet run --urls http://0.0.0.0:5148`.
- Repo: `github.com/Frosk-Kristian/HH-2026-WebServer`. Contract:
  `docs/SERVER_CONTRACT.md`. Transport: `firmware/src/client.cpp`.
- Verified live on 18 Sep: returns `audio/wav`, 1 channel, 24000 Hz, 16-bit.
  2.3 s for a short label, 9.9 s for a 12-second reading — so the 30 s
  `SERVER_TIMEOUT_MS` matters and HTTPClient's 5 s default does not do.

The mock server and the OpenRouter direct path were **deleted** on 18 Sep, not
flagged off (D29). There is no `USE_MOCK_SERVER`, no `USE_LOCAL_SERVER`, and no
`#if` anywhere choosing a backend. The trade-off is real and was accepted: there
is no longer any way to exercise the press path with no server and no internet.

## Who owns what

- **Sam (this repo's main user)** — `main.cpp`, `buttons`, `audio`, `phrase`,
  `reader`. All working.
- **Other SWE** — `camera.cpp` ✅ **written and confirmed on hardware 8 Sep**
  (OV3660 detected, 136 ms capture, no leak). `vision.cpp` is **no longer
  needed at all** — the server does vision, so nothing calls `vision_read()`
  any more (D29). `pipeline.h` is still the agreed contract for the camera
  half; `pipeline_stub.cpp` keeps its weak symbols and `camera.cpp`'s strong
  ones override them.
  ⚠️ `camera_release()` calls `free()`, **not** `esp_camera_fb_return()` — the
  buffer is ours, not the driver's. See D26 and the header of `camera.cpp`.
- **Kristian** — the ASP.NET server.
- **Mechatronics** — lens refocus, LEDs, enclosure, power.

## Conventions

- One decision per line in `docs/decisions.md`, dated. Record the *why*.
- Never commit `config.h` or an API key.
- `phrases.h` is generated — never hand-edit.
- The device must never be silent: every failure path plays a phrase.
