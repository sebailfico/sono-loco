# Hardware Bench Test

**The ESP-NOW mesh path works.** First proven 2026-08-19: a WROOM sourcing and an
ESP32-S3 playing, 132,069 packets over 600 s with zero lost, zero overflow, zero
underrun, zero duplicates and zero resyncs. What remains unproven is the
*Bluetooth* half of the server role — audio taken from a phone and forwarded —
because no board on the bench can do it (see "What you need" below).

There are two ways to test. Use the automated one by default.

## Automated: `tools/bench-mesh.ps1`

```powershell
./tools/bench-mesh.ps1 -Flash -Duration 120
```

It discovers every ESP32 attached to the PC, identifies each by chip, flashes the
matching firmware, reboots them all into bench mode, elects one to generate a
synthetic test stream, collects telemetry, and reports stream health and clock
drift. There is no board limit — a third node is picked up automatically.

It works around the fact that the normal SERVER role needs a phone: bench mode
generates the stream itself and never starts Bluetooth. That is also why a WROOM
can take part (see D3).

What to read in its output:

- **Firmware line** — the `git describe` version each node is actually running,
  printed at the top of the run and again with the results. Warnings here come
  before anything else in the output for a reason: a board left running an older
  build, or a build made from a dirty tree, produces numbers that will not
  reproduce, and nothing else on screen would reveal it. Rerun with `-Flash` if
  a node does not match the tree, and commit before measuring anything you intend
  to record. See D10.
- **Stream table** — `lost`, `ovf`, `und`, `dup`, `rsy` should all be 0. `rx`
  should be within a few packets of the source's `tx`.
- **Source line** — packets per second should be 220.5. `qfull`, `senderr` and
  `radiofail` at 0 mean the radio kept up.
- **Clock drift** — three measures. `log ppm` regresses each node's `millis()`
  against PC time; `audio ppm` derives the same thing from how fast the jitter
  buffer fills or empties; `corr` counts the samples the drift controller
  inserted or dropped. Prefer the audio column over the log one: it measures the
  drift that actually causes dropouts, and it is immune to the serial latency
  jitter that makes the log column useless on native-USB boards. The script says
  so itself when a figure is below its own noise floor.
- **Which column is the measurement depends on whether correction is on.** With
  correction running, a flat `audio ppm` is the *result*, not the measurement —
  the drift has been moved into the `corr` column, one sample at a time. Read it
  as `+2418/-0`: inserts and drops are shown separately because a controller
  doing both in equal measure is hunting rather than correcting, and a net figure
  would hide that.

  To measure the raw drift instead, run with `-NoDrift`. Doing both back to back
  on the same boards in the same session is the only comparison worth recording:
  the offset is a property of that pair of crystals at that temperature, so a
  corrected run today against an uncorrected one from last week proves nothing.
- **Time to exhaustion** — at the measured drift, how long before the jitter
  buffer overflows or underruns. This is the number that matters for the
  clock-drift work in `TODO.md`.

Drift precision improves with run length. 45 s is enough to see whether audio
flows; use 600 s or more before trusting a ppm figure.

## Manual walkthrough

The procedure below is the original by-hand version. It is still the only way to
test the Bluetooth path, and worth doing once so the log output is familiar.
Each step has a pass criterion readable directly from the serial log.

## What you need

For the **automated** mesh test: two or more boards on this PC, any mix. Bench
mode does not use Bluetooth, so a WROOM counts.

For the **manual** test of the Bluetooth path: at least one **WROVER**, plus a
phone. Neither board currently on the bench can be an A2DP server — the WROOM has
no PSRAM (D3) and the S3 has no BT Classic — so that half cannot be tested until
a WROVER is bought. This is the outstanding hardware purchase in `TODO.md`.

A PCM5102 on each node if you want to hear anything; the counters work without
one.

Known bench hardware as of 2026-08-19 (`pio device list`, confirmed by
`esptool chip_id`):

| Port | Chip | Module | Role in a bench run |
|------|------|--------|---------------------|
| COM8 | ESP32-D0WD-V3 | WROOM, no PSRAM | source or client, bench mode only |
| COM9 | ESP32-S3 | 8 MB embedded PSRAM | source or client |

Note the S3 enumerates on its **native USB** port (VID 303A), not a UART bridge,
which is why the build sets `-DARDUINO_USB_CDC_ON_BOOT=1`. Without that flag
`Serial` goes to GPIO43/44 and the board is silent over USB.

Capture a manual run so two runs can be compared:

```
./tools/capture-serial.ps1 -Environment esp32wrover
```

---

## Step 0 — Host tests still pass

```
pio test -e native
```

Or, without a host compiler, on a connected board:

```
pio test -e esp32dev
```

**Pass:** all tests OK. If the ring buffer or sequence accounting is broken there
is no point looking at radios yet.

---

## Step 1 — Boot, heap and PSRAM report

Flash and monitor each board on its own.

```
pio run -e esp32wrover --target upload
pio device monitor -e esp32wrover
```

**Expect:**

```
  SonoLoco — Multi-Room Audio
  Firmware: v0.1.0
  Mode: SERVER capable (BT + ESP-NOW)
[INFO]  Chip: ...
[INFO]  PSRAM: 4194304 bytes
[INFO]  Heap after WiFi init: ...
[INFO]  ESP-NOW ready — MAC: XX:XX:XX:XX:XX:XX
[INFO]  Heap after ESP-NOW init: ...
[INFO]  BT discoverable as: SonoLoco-WROVER
[INFO]  Setup complete. Entering DISCOVERY...
```

**Pass criteria:**

- `PSRAM:` is non-zero on the WROVER. If it reports 0, the mesh is disabled by
  design and everything after this step will fail — fix that before continuing.
- `Heap after ESP-NOW init` is comfortably above zero. This is the number the
  whole WROOM exclusion is about; write it down, it is the baseline for judging
  whether any later change is affordable.
- A WROOM instead prints `No PSRAM detected — WiFi/ESP-NOW disabled` and that is
  correct behaviour, not a failure.
- Record each board's MAC. The client locks onto the server's MAC, so knowing
  which is which makes the next steps readable.
- `Firmware:` shows the commit the board was built from, and no trailing `*`.
  A `*` means uncommitted changes, so the run cannot be reproduced from the sha
  and is not worth recording.

---

## Step 2 — DISCOVERY → SERVER on phone connect

One board only. Connect the phone to `SonoLoco-WROVER` and start playing audio.

**Expect:** `BT connected`, then `=== SERVER — local playback + ESP-NOW
broadcast ===`, then `BT audio started → ESP-NOW TX will activate in 1000ms`.

**Pass criteria:**

- Audio plays locally out of the DAC.
- The connect tone plays without distorting the music that follows.
- Status lines every 10 s show `mode=SERVER` with **`qfull=0 senderr=0
  radiofail=0`**. Any of those climbing means the radio cannot keep up with
  ~220 packets/s and nothing downstream will be trustworthy.

`radiofail` in particular counts frames the radio reported as not delivered. With
no client listening yet, broadcast frames are not acknowledged, so treat a
non-zero value here as informational and re-read it in step 3.

---

## Step 3 — Second board reaches CLIENT and holds

Power the second board with the first still serving.

**Expect on the second board:**

```
[INFO]  === CLIENT — ESP-NOW → I2S ===
[INFO]  Stopping BT to release I2S...
[INFO]  I2S initialised for CLIENT mode at 22050 Hz mono
[INFO]  Jitter buffer ready (~4000 bytes) — starting I2S
```

**Pass criteria:**

- It reaches `Jitter buffer ready` within a few seconds of the server streaming.
- Audio comes out of the client's DAC.
- `jitter=` in the status line **holds roughly steady**. This is the single most
  informative number on the bench:
  - steadily climbing → the client consumes slower than the server sends, and it
    will eventually overflow
  - steadily falling → the opposite, and it will underrun
  - either drift is the clock-drift problem in `TODO.md`, not a wiring fault
- `und=` (underrun) and `ovf=` (overflow) stay at 0 over a minute.

---

## Step 4 — Five-minute stream

Leave it playing for five minutes and watch the status lines.

**Pass criteria:**

- `lost=` grows slowly if at all. A steady climb means the radio is saturated —
  ADPCM compression is the planned answer.
- `ovf=` and `und=` stay at 0. Either one climbing while `lost` stays flat is a
  timing problem, not a radio problem.
- `dup=` and `rsy=` stay at 0. A non-zero `rsy` with no server restart means
  packets are arriving out of order, which the sequence tracker handles but which
  says something about the radio conditions.
- `heap=` is flat. A slow decline is a leak; the `String` concatenation in the
  log macros is the first suspect.

Note explicitly whether the two rooms sound aligned. They are expected **not**
to: the server plays through A2DP at tens of ms latency, the client after a
~137 ms of buffering. Record the perceived offset — it is the input to the
timing work.

---

## Step 5 — Teardown and recovery

The transitions that have historically broken:

1. **Disconnect the phone.** Both boards should print `=== DISCOVERY ===`, the
   client after `ESP-NOW silent for 5s → DISCOVERY`.
2. **Reconnect the phone to the same node.** It must become SERVER again. This is
   what `a2dpSink.end(false)` exists for; if it fails here, the controller was
   freed.
3. **Now connect the phone to the *other* node.** The roles must swap. This is
   the whole premise of the project and the step most likely to expose a stale
   `senderLocked` — a node that ignores every future server.
4. **Reboot the server mid-stream.** The client should log `rsy=1` and keep
   playing, not report tens of thousands of lost packets.

**Pass:** all four, repeatedly, with no power cycle.

---

## Recording the result

Keep the captured logs. When a change later makes something better or worse, the
only way to tell is to compare the counters between two runs of this same
procedure, so record the firmware version alongside the numbers in
`CHANGELOG.md`.

`tools/bench-mesh.ps1` writes that version into its log header for you:

```
# SonoLoco mesh bench
# started  : 2026-08-20 11:04:12
# version  : v0.1.0-3-gabc1234
# duration : 600 s
# source   : COM8
# nodes    : COM8=ESP32-D0WD-V3 COM9=ESP32-S3
# firmware : COM8=v0.1.0-3-gabc1234 COM9=v0.1.0-3-gabc1234
```

A manual capture has no such header, so write the version down from the boot
banner. A measurement whose build cannot be identified is an anecdote.
