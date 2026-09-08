# Server contract — what the device sends, what it needs back

**For Kristian.** You should not have to read any firmware to build against
this. Everything the device requires is on this page, and every claim here
points at the line that enforces it.

Written 8 Sep 2026. If this page and the firmware ever disagree,
[`firmware/src/reader.cpp`](../firmware/src/reader.cpp) wins — tell Sam and
this page gets fixed.

---

## 1. What the device actually does

Five lines, so the rest makes sense.

1. Someone with vision impairment points the device at a medicine label and
   presses a button. They **cannot see the result and cannot check it.**
2. The camera captures a JPEG to PSRAM. ~27 KB, SVGA, ~136 ms. Working.
3. It POSTs that JPEG to **your server** and expects **audio** back.
4. It streams the audio straight into the I²S DAC as it arrives, and speaks it.
5. If anything fails, it plays a short pre-recorded phrase instead. It must
   never be silent.

Your server is the entire middle. It does the vision call **and** the
text-to-speech, and hands back finished audio — one round trip. That is
deliberate: it keeps TLS off the device (our biggest crash risk) and keeps the
OpenRouter API key off device flash. See **D25** in
[`decisions.md`](decisions.md).

---

## 2. The request

```
POST http://<laptop>:5148/api/tts/fromimage
Content-Type: image/jpeg

<raw JPEG bytes>
```

- **Raw bytes in the body.** Not multipart, not base64, not JSON.
- Typically **20–35 KB**. Allow up to ~200 KB.
- No auth header. Plain HTTP on the LAN, no TLS — deliberate, see D25.
- The device waits **30 s** before giving up
  (`SERVER_TIMEOUT_MS`). Vision + TTS inside that is the whole budget.

Enforced at [`reader.cpp:137-142`](../firmware/src/reader.cpp#L137).

### Only one endpoint exists, and it means "read"

All three modes currently POST to the same path — `config.h` sets
`SERVER_READ_PATH`, `SERVER_SUMMARISE_PATH` and `SERVER_DESCRIBE_PATH` to
`/api/tts/fromimage`, and [`reader.cpp:29`](../firmware/src/reader.cpp#L29)
just picks between those constants.

**So right now the server has no way to tell which mode was pressed.** That is
fine — the agreed cut list is **Mode A (Read) + Repeat**, and everything else
is cut. Treat every request as "read".

If we ever want describe/summarise back, it is a one-line change on our side to
send different paths. Don't build for it now.

---

## 3. The response — this is the part with the sharp edges

### On success: HTTP 200, body = audio

The device is deliberately tolerant here, because your server was being written
in parallel with the firmware. It sniffs the body at
[`reader.cpp:58`](../firmware/src/reader.cpp#L58):

| You send | Works? |
|---|---|
| Bare PCM, no header | ✅ |
| RIFF/WAVE with a `fmt ` and `data` chunk | ✅ — it walks the chunks |
| 16-bit signed int samples | ✅ |
| 32-bit IEEE float samples | ✅ |

> **This means "return raw int16 PCM instead of a WAV" is NOT required.**
> It was on your list of four changes. A 24 kHz mono WAV works today, as-is.
> Raw PCM saves 44 bytes and a little parsing — worth doing eventually, not
> worth doing before the demo works. **Deprioritise it.**

### Two things that ARE required

These are not tolerated, and one of them fails silently.

| Requirement | What happens otherwise |
|---|---|
| **24000 Hz** | There is no resampler on the device. It logs a warning and plays the speech at the wrong speed. [`reader.cpp:161`](../firmware/src/reader.cpp#L161) |
| **Mono, 1 channel** | ⚠️ **Silent failure.** `fmt.channels` is parsed but never used to de-interleave, so a stereo body plays as noise at double rate with no error anywhere. |

So: **24 kHz, mono.** 16-bit signed is the natural choice.

Streaming the body as you generate it is welcome — the device plays it as it
arrives and reports time-to-first-audio-byte separately. Chunked transfer is
fine; it does not require a `Content-Length`.

### On failure: the status code is the only thing we can hear

The device never sees your text. On a non-200 it plays a phrase and skips the
audio entirely, so **the status code you pick is the error message the user
hears.** Mapping is at
[`main.cpp:130`](../firmware/src/main.cpp#L130):

| You return | User hears |
|---|---|
| **200** | the speech |
| **422** | *"I could not find any text."* — use this when the image is fine but has no legible text |
| **502** / **503** | *"No internet connection."* — use when **you** could not reach OpenRouter |
| any other 4xx/5xx | a generic error phrase |
| *no response at all* | *"No internet connection"* |

⚠️ **Do not invent a status code for "the numbers might be wrong."** A non-200
suppresses the audio, so returning e.g. 409 for an uncertain reading would
throw away the very reading the user asked for. Uncertainty goes **into the
speech**, via the prompt below.

---

## 4. The prompt — copy it exactly, and understand why

Live copy: `PROMPTS["read"]` at
[`tools/reference_pipeline.py:54`](../tools/reference_pipeline.py#L54). Copy
from there rather than retyping from here, in case it has moved on.

```
Transcribe all text visible in this image, exactly as written. Preserve the
reading order a sighted person would use. Output ONLY the transcribed text --
no preamble, no description, no commentary, no markdown. Never guess a
character you cannot clearly see. Mark any digit you cannot read with
certainty as [?]. After any dose, quantity, date, time or amount of money that
you cannot read with full certainty, write UNCLEAR. It is far better to say
UNCLEAR than to guess a number. If the image contains no legible text, output
exactly: NOTEXT
```

**Why those last three sentences exist (D17).** On a deliberately degraded
label, *every* candidate vision model confidently invented numbers rather than
admitting doubt: a quantity of 56 read as 60 by all four, an expiry date
returned as `09/31/26` — a date that does not exist — and one model fabricated
an entire distributor line that appears nowhere in the image.

For a device that reads medicine labels aloud to someone who cannot check the
result, that is the most dangerous failure mode in the project. The wording
does not make the numbers right. It makes the device honest about which ones
to trust.

**Two couplings you must not break:**

- **`NOTEXT`** — if the model returns exactly this, return **422** and no
  audio. Don't send "NOTEXT" to TTS; it costs money and sounds like a fault.
- **`UNCLEAR` and `[?]`** — [`main.cpp:105`](../firmware/src/main.cpp#L105)
  scans the text for these literal strings and plays a warning before the
  reading. **If you reword the prompt so the model stops emitting those exact
  tokens, the safety warning silently stops working.**

Vision model: currently `google/gemini-3-flash-preview`. **Do not switch it**
— D16 has a candidate that is 1.4 s faster and 37% cheaper, but it is gated on
a 20-photo eval that has not been shot yet.

---

## 5. Where everything is

The file map, since it is spread out.

| What you need | Where |
|---|---|
| **The prompts** | [`tools/reference_pipeline.py:44`](../tools/reference_pipeline.py#L44) — `PROMPTS` |
| **A working end-to-end reference in Python** | [`tools/reference_pipeline.py`](../tools/reference_pipeline.py) — this is ground truth. If the device disagrees with this script, the device is wrong |
| **The client side of this contract** | [`firmware/src/reader.cpp`](../firmware/src/reader.cpp) — `read_aloud()` |
| **The status→phrase mapping** | [`firmware/src/main.cpp:130`](../firmware/src/main.cpp#L130) — `phrase_for_status()` |
| **The mock server** | [`tools/mock_server.py`](../tools/mock_server.py) — speaks the *old* OpenRouter shape, not yours |
| **The conformance suite** | [`tools/test_mock.py`](../tools/test_mock.py) — 21 assertions |
| **Why anything is the way it is** | [`docs/decisions.md`](decisions.md) — D14, D15, D17, D25 are the ones that concern you |
| **Where things stand today** | [`docs/TODAY.md`](TODAY.md) |
| **Your repo** | `github.com/Frosk-Kristian/HH-2026-WebServer` |

Worth ten minutes of your time, in this order: **D25** (why your server
exists), **D17** (why the prompt is worded like that), **D15** (why 24 kHz).

---

## 6. Running it on Sam's laptop

The server runs on **Sam's laptop**, because Sam is also the hotspot and the
board has to reach it. Checked 8 Sep — **there is no .NET SDK there**, so
`dotnet run` will not work. But the runtimes are installed:

```
Microsoft.AspNetCore.App  9.0.10
Microsoft.NETCore.App     8.0.18 / 9.0.10 / 10.0.3
```

Note what is and is not there: **`Microsoft.AspNetCore.App` exists only at
9.0.10.** `Microsoft.NETCore.App 8.0.18` is the *base* runtime, not the ASP.NET
one — so a framework-dependent **net8.0 web app will not start here**, even
though an 8.x runtime appears in the list. Machine is `win-x64`.

### Route 1 — self-contained publish ← do this

Works no matter which framework version you target, and needs nothing installed
on Sam's machine. One command, from whatever SDK you already have:

```
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

Send Sam the output folder. He runs:

```
.\HH-2026-WebServer.exe --urls http://0.0.0.0:5148
```

~70–150 MB, so **move it on a USB stick, not over the hotspot** — the phone is
carrying the board's connection. This is also exactly what we want on the demo
laptop, because it cannot be broken by a runtime mismatch or by your laptop
being in another room.

### Route 2 — zero-download stopgap, 30 seconds

If you already have a framework-dependent build to hand, try forcing it across
the major version rather than republishing:

```
dotnet --roll-forward Major HH-2026-WebServer.dll --urls http://0.0.0.0:5148
```

net8 → net9 is usually fine for a simple API. Worth one try before copying
100 MB around; if it throws, go to Route 1.

### Route 3 — install the ASP.NET Core 8 runtime on Sam's laptop

~8 MB, not the 200 MB SDK. Makes an existing net8.0 framework-dependent publish
just work. Sensible if you would rather not republish each time you change
something.

Sam is installing the full SDK as well, so he can eventually
`git pull && dotnet run` while you iterate — but **none of the routes above
wait on that.**

### ⚠️ `--urls http://0.0.0.0:5148` is not optional

ASP.NET's default profile binds to `localhost`, which means the loopback
adapter only. **The ESP32 cannot reach that** — it will get connection refused
and the device will announce "No internet connection" while sitting on perfect
Wi-Fi. Bind to `0.0.0.0`.

Also: Windows Firewall will prompt on first bind. Allow it on **Private**
networks. If it was already denied, the rule has to be removed by hand — and
note that if Avast is installed it registers as the system firewall, so the
Windows rules look right and are ignored (this cost us a day on 4 Sep).

---

## 7. Testing without the device

```bash
# 1. Is it reachable from another machine at all?
curl -v --data-binary @photo.jpg -H "Content-Type: image/jpeg" \
     http://<sam-laptop-ip>:5148/api/tts/fromimage --output out.pcm

# 2. Is the audio the right shape?  Must be 24000 Hz, 1 channel.
ffprobe out.pcm            # or open it in Audacity as raw 16-bit LE mono 24k

# 3. Does the vision half agree with ground truth?
python tools/reference_pipeline.py photo.jpg --mode read --text-only
```

If `out.pcm` plays back as recognisable speech at the right pitch in Audacity,
the device will play it correctly. Wrong pitch means the sample rate is wrong;
noise means it is probably stereo.

---

## 8. Your four changes, re-scoped

| # | Change | Verdict |
|---|---|---|
| 1 | **Use the real prompt** from `PROMPTS["read"]` | 🔴 **Do it.** Without it there is no `NOTEXT` and no `UNCLEAR`, so the safety warning cannot fire |
| 2 | **Bind `--urls http://0.0.0.0:5148`** | 🔴 **Do it.** Nothing works at all without this |
| 3 | **Real status codes** — 422 for no text, 502/503 for upstream failure | 🔴 **Do it.** This is the device's entire error vocabulary |
| 4 | ~~Return raw int16 PCM instead of a WAV~~ | 🟢 **Not needed.** The device already handles WAV. Just make sure it is **24 kHz mono** |

Three of four, and one of them is a command-line flag.
