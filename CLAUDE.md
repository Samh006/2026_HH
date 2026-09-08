# Talking Reader

Handheld assistive device: point it at a medicine label, press a button, it
reads the text aloud. ESP32-S3, camera, two buttons, a speaker, no screen.
Users have vision impairment and cannot check what the device tells them —
which is why wrong numbers matter more than anything else here.

**Showcase: 15 Sep 2026.**

## Read before doing anything

1. `docs/decisions.md` — D1–D27. The source of truth. Records *why*, including
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
   in Wi-Fi, key and this laptop's LAN IP. Generate phrases with
   `python tools/wav_to_phrases.py --emit-header`.

5. **Campus Wi-Fi ("Curtin") is WPA2-Enterprise and the ESP32 cannot join it.**
   Our code only does pre-shared keys. Use a phone hotspot, 2.4 GHz, and both
   the laptop and the board must be on it.

## Commands

```bash
cd firmware
python -m platformio run -e freenove_s3 -t upload   # build + flash
python -m platformio device monitor -b 115200       # watch (UART port)
python -m platformio run -e smoke -t upload         # minimal liveness test

python tools/mock_server.py                          # offline fallback backend
python tools/test_mock.py                            # 21 conformance assertions
```

## Backends — chosen in `config.h`

| | |
|---|---|
| `USE_LOCAL_SERVER 1` | **Current.** Our ASP.NET server: POST a raw JPEG to `/api/tts/fromimage`, get audio back. No TLS, no base64, no SSE. See `firmware/src/reader.cpp`. Repo: `github.com/Frosk-Kristian/HH-2026-WebServer` |
| `USE_MOCK_SERVER 1` | `tools/mock_server.py`. Canned answers, works with no internet. **The demo-day fallback — keep it working.** |
| both `0` | OpenRouter direct. Needs TLS on device; never tested. |

## Who owns what

- **Sam (this repo's main user)** — `main.cpp`, `buttons`, `audio`, `phrase`,
  `reader`. All working.
- **Other SWE** — `camera.cpp` ✅ **written and confirmed on hardware 8 Sep**
  (OV3660 detected, 136 ms capture, no leak). `vision.cpp` **still unwritten**,
  and only needed if `USE_LOCAL_SERVER` goes back to 0.
  `pipeline.h` is the agreed contract; `pipeline_stub.cpp` provides weak
  symbols so the two halves cannot collide. **Keep the stub** until `vision.cpp`
  exists — `camera.cpp`'s strong symbols already override its half.
  ⚠️ `camera_release()` calls `free()`, **not** `esp_camera_fb_return()` — the
  buffer is ours, not the driver's. See D26 and the header of `camera.cpp`.
- **Kristian** — the ASP.NET server.
- **Mechatronics** — lens refocus, LEDs, enclosure, power.

## Conventions

- One decision per line in `docs/decisions.md`, dated. Record the *why*.
- Never commit `config.h` or an API key.
- `phrases.h` is generated — never hand-edit.
- The device must never be silent: every failure path plays a phrase.
