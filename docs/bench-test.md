# Hardware Bench Test

The mesh path has never run on hardware. It compiles for all three environments
and the pure-logic parts are covered by host tests, but nothing below the
`lib/jitter` line has been observed working on a board.

This is the procedure for the first two-board bring-up, written in advance so it
does not have to be invented while staring at a serial monitor. Work through it
in order; each step has a pass criterion readable directly from the log.

## What you need

- Two boards on this PC, each on its own COM port. At least one **WROVER** — a
  WROOM cannot be a mesh server (see D3 in `docs/decisions.md`), so a
  WROOM+WROOM pair cannot test any of this.
- A PCM5102 wired to each, per the README wiring diagram.
- A phone that can connect to a Bluetooth speaker.

Verify the ports first — the values in `platformio.ini` are guesses:

```
pio device list
```

Update `upload_port` / `monitor_port` in `platformio.ini` if they differ. Capture
each run so two runs can be compared:

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
[INFO]  Jitter buffer ready (~2000 bytes) — starting I2S
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
~185 ms jitter buffer. Record the perceived offset — it is the input to the
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
procedure, so note in `CHANGELOG.md` which firmware state a capture belongs to.
