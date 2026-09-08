# Today — 8 Sep 2026 · full team present, in person

**Day 5 of 11.** Freeze 12 Sep — **four days.** Showcase 15 Sep.

> ⚠️ **The dates disagree across our own documents and nobody has settled it.**
> `CLAUDE.md` says showcase 15 Sep. The 4 Sep plan said ship 14 Sep, freeze
> 12 Sep. Pick one today and write it in `CLAUDE.md`, because "four days" and
> "seven days" are different projects.

Day 1 (4 Sep) got the week-1 milestone: button press → speech. Everything
between the camera and the speaker has worked since then. **Days 2–4 are
unrecorded** — this file is the first session log since.

> **Today's win condition:** one real photo, read aloud, end to end.
> camera captures → POST to the ASP.NET server → speech out of the speaker.

---

## The four things that have to land

| # | Item | Status |
|---|---|---|
| 1 | **`camera.cpp`** — JPEG to PSRAM behind the `pipeline.h` contract | ✅ **DONE, verified on hardware** |
| 2 | **Kristian's four server changes** | ⬜ not started as of this write-up |
| 3 | **Hotspot + `SERVER_BASE_URL`** | ✅ **correct** — hotspot up, board joined, URL points at the right laptop |
| 4 | **Flash and test** | 🟡 flashed and tested up to the server, which refused |

### 1. camera.cpp — done ✅

Written, built, flashed, and confirmed against the physical sensor.

```
[cam ] sensor PID=0x3660 (OV3660)
[cam ] tuned: ae_level=-1 gainceiling=16x denoise=4 sharpness=2
[cam ] ready: q12 fb_count=2 psram_free=8181959
[cam ] frame 0: 26392 bytes   frame 1: 27256   frame 2: 27281  <- sharpest
[cam ] kept 27281 bytes of 3 candidates
[cam ] captured 27281 bytes in 254 ms
```

- **The camera pin map is now confirmed on silicon**, not just on paper. The
  sensor answers on SCCB and identifies as `0x3660`. That closes an open
  question that has been sitting in `wiring.md` since day one.
- `camera_tuning.h`'s best-of-3 selection works as designed — three candidates,
  the largest kept.
- **Capture costs 136 ms** on a warm sensor (254 ms on the first press, which
  includes lazy init). The budget allowed ~0.95 s for the whole device side, so
  this is well inside it.
- **No memory leak.** Two consecutive presses returned free heap to exactly
  `250820` and PSRAM to `8181687` both times. See D26 for why that was the part
  worth getting right.
- The `*** STUBBED ***` banner is gone on the `USE_LOCAL_SERVER` path — the
  weak symbols in `pipeline_stub.cpp` lost to the real ones at link time,
  exactly as `pipeline.h` intended. No flag, no `#ifdef`, no edit to `main.cpp`.

**`pipeline_stub.cpp` still exists and should stay** until `vision.cpp` is
written or the two-leg path is dropped for good.

### 3. Hotspot and `SERVER_BASE_URL` — correct ✅

```c
#define SERVER_BASE_URL "http://172.20.10.4:5148"
```

`172.20.10.4` is **Sam's laptop, which is where the server will run.** Sam is
also the one hotspotting. So the config is right and nothing needs changing —
the address is simply not answering yet, because the server is not running.

Hotspot: 2.4 GHz, board associated at `172.20.10.6`, RSSI −35.

⚠️ **`172.20.10.4` is a DHCP lease from the phone, not a fixed address.** If the
laptop leaves the hotspot and rejoins — to grab something over campus Wi-Fi,
say — it can come back on a different `172.20.10.x`. Re-check with `ipconfig`
after any network switch, because the symptom is identical to the server being
down and it costs a reflash.

#### What running it here actually needs

Checked on 8 Sep. **The .NET SDK is not installed on this laptop**, so
`dotnet run` — the command in `config.h`'s own comment — will fail:

```
No .NET SDKs were found.
```

But the **runtimes are** there, and one of them is the right one:

```
Microsoft.AspNetCore.App  9.0.10          <- this is the one that matters
Microsoft.NETCore.App     8.0.18 / 9.0.10 / 10.0.3
```

So there are three ways in, cheapest first:

1. **Kristian publishes framework-dependent, targeting `net9.0`.** Copy the
   folder over, run `dotnet HH-2026-WebServer.dll --urls http://0.0.0.0:5148`.
   A few MB, no download, works with what is already installed today.
   ⚠️ It must be **net9.0** — `Microsoft.AspNetCore.App` is only present at
   9.0.10, and .NET does not roll a net8.0 app forward a major version by
   default.
2. **Kristian publishes self-contained** (`-r win-x64 --self-contained`).
   ~70 MB, no runtime dependency at all, copies by USB stick. **This is the one
   to have on the laptop for demo day**, because it cannot be broken by a
   runtime mismatch or by Kristian's laptop being elsewhere.
3. **Install the .NET SDK here** (~200 MB). Worth doing *as well*, because
   Kristian is iterating on four changes today and re-publishing for each one
   is friction — with the SDK it is `git pull && dotnet run`.
   **Download it over campus Wi-Fi or Ethernet, not the hotspot** — the phone
   is carrying the board's connection and 200 MB of mobile data is a poor
   trade. Then rejoin the hotspot and re-check the IP.

There is no local checkout of `HH-2026-WebServer` on this machine either, so
that needs cloning whichever route is taken.

### 4. Flashed and tested — the chain works up to the server 🟡

Full press-to-failure path, on real hardware:

```
[btn ] A short -> READ  ->  wifi ok  ->  capture 27243 bytes in 136 ms
  ->  POST /api/tts/fromimage  ->  connection refused  ->  HTTP -1
  ->  [phr ] no_internet
```

Everything the device owns works. It is waiting on a server to answer.

---

## Two live hazards found today

**1. The device says "No internet connection" when the *server* is down.**
`phrase_for_status()` in `main.cpp` maps every transport-level failure — which
includes *server refused* — to `PH_NO_INTERNET`. Observed today on Wi-Fi at
RSSI −35. This is D24's failure wearing different clothes: if Kristian's laptop
sleeps during the showcase, the device blames the network in front of judges.
A distinct phrase for "cannot reach the server" would fix it, and it is
blocked on the same voice decision as everything else.

**2. Seven of ten phrases are still unrecorded, and the device proves it.**

```
[phr ] missing: reading describing connecting batt_low error repeating uncertain  (7 of 10)
[phr ] MISSING 'reading' -- device is silent here.
```

`error` and `uncertain` are both in that list. **Right now, if the camera
fails, or the model cannot read the label, the device makes no sound at all** —
indistinguishable from a flat battery to a user who cannot see it. `CLAUDE.md`
states "the device must never be silent: every failure path plays a phrase."
That rule is currently violated on most paths.

---

## Three things that need people in a room — open since 31 Aug

**All three are still open on day 5.** Everyone is physically present today;
that is the resource these have been waiting for, and it is not guaranteed
again before freeze.

- [ ] 🔴 **Voice cloning: Route A or Route B?** Route A = accept the
      `kyutai/pocket-tts` HF terms and keep D6's one-voice goal. Route B = pick
      a catalog voice and match the OpenRouter voice to it, reversing D13's
      ordering. **Both are one flag apart in `make_phrases.py`.** Eight days
      open, blocking 7 of 10 phrases including the D17 safety warning. Every
      silent failure path traces back to this one decision.
- [ ] 🔴 **Book 3–4 user testers.** Phone calls, not messages. Booked later
      than today and they probably cannot make the date.
- [ ] 🔴 **Lens refocus + the two diffused LEDs.** D18 says these are the
      primary accuracy lever, ahead of any model choice, and they are also the
      fix for the motion blur `camera_tuning.h` is currently working around in
      software. Five minutes for the lens.

---

## Also landed today

- **`hardware/wiring.md` rewritten for the S3** (D27). It was still the WROVER
  document: buttons on GPIO 13/15 (which are the camera's PCLK and XCLK here)
  and I²S on 32/33/14 (not on this header at all). Its "do not use" table
  called GPIO 16/17 the PSRAM bus when they are camera data lines, and listed
  GPIO 21 as camera bus when it is button B. `CLAUDE.md` names that file as the
  live pin map, so anyone wiring from it today would have broken the camera and
  hunted for the fault in software.
- **D25** — the parked self-hosted-server question is closed as *adopted*. The
  firmware for it shipped in `c3aafe9` on 4 Sep while the log still read
  "nothing has been committed and no firmware has been changed for it."
- **D26** — `camera_release()` frees its own buffer rather than returning a
  framebuffer, and why the `pipeline.h` wording was left alone.
- **The boot banner reports the wrong backend.** Both `USE_MOCK_SERVER` and
  `USE_LOCAL_SERVER` are `1`; the banner checks the first and prints
  `MOCK http://172.20.10.4:8080/api/v1`, while the code takes the second to
  port 5148. One line in `main.cpp`. Not fixed — it is Sam's file.

---

## Measured today

| | |
|---|---|
| JPEG size, SVGA q12, indoor | **27.2 KB** (26.4–27.3 across candidates) |
| Capture, warm sensor | **136 ms** |
| Capture, first press incl. lazy init | **254 ms** |
| Free heap after capture | 251,068 (min 249,260) |
| Free PSRAM after capture | 8,154,659 of 8,386,019 |
| Heap across two presses | **no leak** — identical both times |
| Wi-Fi RSSI on hotspot | −35 |
| Build | RAM 17.0%, flash 32.1% |

---

## Still open, most urgent first

1. 🔴 **The voice-cloning decision.** Day 5. Blocking 7 phrases; makes most
   failure paths silent, including the D17 safety warning.
2. 🔴 **Kristian's four server changes** — real prompt from
   `reference_pipeline.py` `PROMPTS["read"]`, raw int16 PCM not a WAV,
   `--urls http://0.0.0.0:5148`, real status codes.
3. 🔴 **Get the server running on this laptop** — `SERVER_BASE_URL` is already
   correct, but there is no .NET SDK and no checkout here. Fastest route is a
   framework-dependent `net9.0` publish from Kristian; a self-contained publish
   is the one to keep for demo day.
4. 🔴 **Eval photos: still 0 of 10.** Longest lead time in the project. The
   camera works now, and the mock saves every upload to `tools/captures/`, so
   the blocker is a human with real objects — after the lens is refocused.
5. 🔴 **Book the testers.**
6. **Lens refocus + LEDs.**
7. **A distinct "cannot reach the server" phrase**, so the device stops
   blaming the network for a sleeping laptop.
8. **Settle the ship date** — 14 or 15 Sep.
9. **Turn Avast back on and fix it properly** — carried from 4 Sep, still
   listed as left off. If it re-arms as-is, the mock breaks for whoever runs
   the demo fallback.
10. `vision.cpp` — unwritten, and only needed if `USE_LOCAL_SERVER` goes to 0.
11. The 1 Hz waiting tick; offline Repeat by caching PCM in PSRAM.

---

## Previous sessions

Ended sessions move to `docs/sessions/`, so this file is always just today.

- [`docs/sessions/2026-09-04.md`](sessions/2026-09-04.md) — day 1. Button
  press to speech on real hardware, and the five bugs it cost: wrong board,
  wrong flash mode, Avast, a one-space JSON mismatch, and the mock announcing
  "No internet connection" while connected.

---

## Reference — commands

```bash
cd firmware
python -m platformio run -e freenove_s3 -t upload --upload-port COM4
python -m platformio device monitor -b 115200 -p COM4      # CH343 port, D22

python tools/mock_server.py     # demo-day fallback — keep it working
python tools/test_mock.py       # 21 conformance assertions
```
