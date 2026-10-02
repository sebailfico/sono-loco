# Hardware Bench Test

How to test on real boards, in the order to reach for them:

1. **`tools/bench-mesh.ps1`** — the mesh, automated: any number of boards on
   this PC, a synthetic stream, no phone. The default for anything that touches
   the client audio path.
2. **`tools/btlisten/`** — the Bluetooth half, driven from the PC: the PC is the
   phone, and its microphone is the ear.
3. **By hand, with a phone** — the role changes no script can make: connect,
   disconnect, swap servers.

Board names and ports are in the README's build table, MACs in the gitignored
`boards.local.md` beside it; results go in `CHANGELOG.md` with the firmware
version they came from.

## Automated: `tools/bench-mesh.ps1`

```powershell
./tools/bench-mesh.ps1 -Flash -Duration 600 -Mute
```

It discovers every ESP32 attached to the PC, identifies each by chip, flashes the
matching firmware, reboots them all into bench mode, elects one to generate a
synthetic test stream, collects telemetry, and reports stream health and clock
drift. There is no board limit — five at once has been done. Bench mode never
starts Bluetooth (D9), which is also why a WROOM can take part.

**It is loud without `-Mute`.** Every client with an amp plays the bench tone
(440 Hz at about -15 dBFS) for the whole run, and even the MAX98357A that sounds
faint at `listen.py`'s levels is loud with it. With `-Mute` every client zeroes
what it hands to I2S and nothing else: the stream is received, buffered,
drift-corrected and consumed as always. The harness reads each client's `mute=`
before toggling it. Run unmuted only to hear it, and say so before starting one.

**`-Flash` names every classic board `SonoLoco-WROOM`**, because it flashes them
all as `esp32dev`. Before a Bluetooth test, flash the WROVERs by name
(`pio run -e esp32wrover -t upload`) and run the harness without `-Flash`
(`-Source COM22` picks the source). Without `-Flash` it identifies boards from
their own `[BENCH] id` line and never asks esptool to reset anything — which is
also the way round a board whose auto-reset into download mode has become
unreliable ("did not enter download mode"): flash that one by hand, repeating
until "Hash of data verified", or with BOOT held.

What to read in its output:

- **Firmware line** — the `git describe` version each node is actually running,
  printed at the top of the run and again with the results. Warnings here come
  before anything else for a reason: a board left on an older build, or a build
  from a dirty tree (`*`), produces numbers that will not reproduce. Commit
  before measuring anything you intend to record. See D10.
- **Mesh line** — each node's `mesh=` id; the harness warns when they disagree.
  Nodes on different meshes ignore each other by design, and the result is a
  client reporting `rx=0`, exactly like one out of range. `fgn=` in the
  telemetry is the tell — packets heard and dropped as somebody else's.
- **Stream table** — `lost`, `ovf`, `und`, `dup`, `rsy` should all be 0. `rx`
  should be within a few packets of the source's `tx`.
- **Source line** — 386.8 packets/s (44,100 frames/s in blocks of 114).
  `qfull`, `senderr` and `radiofail` at 0 mean the radio kept up.
- **Clock drift** — three measures. `log ppm` regresses each node's `millis()`
  against PC time; `audio ppm` derives the same from how fast the jitter buffer
  fills or empties; `corr` counts the frames the controller inserted or dropped.
  Prefer `audio` to `log`: it measures the drift that causes dropouts, and it is
  immune to the serial latency that makes `log` useless on native-USB boards.
- **Which column is the measurement depends on whether correction is on.** With
  it on, a flat `audio ppm` is the *result*; the drift has moved into `corr`,
  shown as `+1413/-0` because a controller doing both in equal measure is
  hunting, and a net figure would hide that. `-NoDrift` measures the raw drift;
  only a back-to-back pair on the same boards in the same session is worth
  comparing — the offset belongs to that pair of crystals at that temperature.
- **Time to exhaustion** — at the measured drift, how long before the buffer
  overflows or underruns. With correction on it should not appear at all.
- **Re-arm notice** — the buffer columns then describe the longest
  uninterrupted stretch, and the harness says what fraction of the run that
  was: a re-arm steps the level back up, and averaging across it once reported
  -7.6 ppm for a client really drifting at -58. Since D14 one re-arm at the very
  start, the client arming on the source's schedule, is normal.

What good looks like, and the numbers a change to the client path is compared
against, are the latest bench regression in `CHANGELOG.md`. 45 s shows whether
audio flows; 600 s is the shortest run worth quoting a ppm figure from.

## Checking mesh isolation

Two households in one building is the case the mesh id exists for (D12), and it
takes three boards to test: two in one mesh, one pretending to be next door.

1. Put the source and one client on a mesh of their own. In a serial monitor on
   each, type `gcasa rossi` and confirm the `[MESH] mesh=6F59 name=casa rossi`
   line that comes back matches on both.
2. Leave the third board on the default (`gsonoloco` restores it — the reply
   should read `mesh=CF09`).
3. Run the harness as usual. It will warn that the nodes are on different
   meshes, which is the point of this run rather than a problem with it.

**Pass:** the matched pair is as clean as a normal run — zero
`lost/ovf/und/dup/rsy` — and the odd node out reports `rx=0` with `fgn=` climbing
at roughly 387/s. A stationary `fgn=` on that node means it is not hearing the
source at all, which is a different fault and not evidence of isolation.

Then put it back with `gcasa rossi` and watch it join within a few seconds, no
reboot.

To test pairing instead of typing the name, remember it needs a press at **both**
ends: `o` on a node already in the mesh (or a three-second BOOT hold on a
server-capable one) to offer it, then `p` on the node being moved (or a
three-second hold there — GPIO 9 on a C3, GPIO 0 on the others). Expect
`[MESH] offering mesh=…` on one and `[MESH] paired mesh=…` on the other within a
second, plus two beeps at each press and the rising tone on the node that
joined, if it has a DAC. Power-cycle it afterwards to confirm the adopted id
survived: surviving a power cut is the whole reason it is in NVS and not RTC
memory.

Worth checking deliberately, since it is the property the two presses exist for:
with **no** offer open, a listening node must ignore a foreign stream entirely.
Start the source on another mesh, press `p` on the odd node out, and confirm it
reports `listening closed, no offer heard` 60 s later rather than joining.

## The Bluetooth half from the PC: `tools/btlisten/`

The mesh half has a synthetic stream (bench mode); this is the equivalent for
the Bluetooth half, and it needs no phone. The PC is the A2DP source: pair the
server with Windows once, connect it, and it appears as an audio endpoint.

One-time setup, any Python 3 with a venv (the PlatformIO Python has no numpy):

```
python -m venv .venv-btlisten
.venv-btlisten/Scripts/pip install -r tools/btlisten/requirements.txt
```

A server that reboots or is reflashed drops the Bluetooth link; `k` gets it
back without anybody clicking Connect — the board dials the PC, which accepts
as it would a headset. Then set the volume, because a board that dialled in
starts at 1 of 127 and forwards a stream too quiet to hear. The address after
`k` is the PC's own, from `boards.local.md`:

```
python tools/btlisten/ser.py COM20 "kaa:bb:cc:dd:ee:ff
V40
" 8
```

If the dial times out, check that the PC's Bluetooth is switched on.

**`listen.py` — is there a hole in what one node plays?** It plays a 997 Hz
tone into the server and records with the PC's microphone. First the control,
through the laptop's own speaker, then the node:

```
python tools/btlisten/listen.py --device "Speakers (Realtek" --label control
python tools/btlisten/listen.py --serial COM20 --label baseline
python tools/btlisten/listen.py --serial COM20 --pre f --label no-forwarding
```

Each run is ~11 s, writes `logs/listen-<time>-<label>.wav` and a plot beside it,
and prints the server's `a` window for the same seconds. `--pre f` toggles
forwarding before the run (send it again to turn it back on); `--pre w` stops
WiFi until the next reboot; `--at 3.0:j` sends a command three seconds into
playback — `j` plays the connect jingle over the tone. To hear a *client*
instead, mute the others (`m`, reading `?` first) and add `--client COMn` for
each client: its `lost`, `rec` and lost-run lengths for the recorded seconds.

**What clean looks like:** tone **997.00 Hz**, **6.02 s** long, **0 dips** (the
control, and WROVER2 on 2026-09-28). What the crackle looked like, the one time
it was caught (`logs/listen-20260928-170905-crackle-fwd-on.wav`): 27 dips/s
lasting ~3 ms, spaced 24–28 ms — one per A2DP packet from Windows (1024 frames,
23.2 ms) — with the tone at 987 Hz and 6.55 s long. Pitch and length are the
strongest signal: room noise cannot make a tone take half a second longer.

The `a` window, one line per run:

```
[A2DP] win=11.9s pk=467 pk/s=39.4 pkB=4096..4096 gapmax=31.9ms cbmax=1.91ms
       jit=8412 und=0 ovf=0 dry=0 fwd=1 wifi=1 heap=29404
       gap5ms=0,456,9,0,0,0,1,0 tx=17580 ...
```

`gap5ms` is a histogram in 5 ms buckets of the time between one packet from
the Bluetooth stack and the next, and `cbmax` the longest our callback took.
The server plays from its own ring, as a client does (D14), so a gap is
absorbed by the ring (`jit`, bytes) and only a gap longer than it holds is a
hole — `und`, on the server's speaker and a moment later in every room. Windows
logged before `cb355de` measured the library's own I2S output instead, and
carry `ring=`, `late=`, `nb=` and `writemax=`.

**`sync.py` — how far apart do the nodes play?** Clicks into the server, one
node unmuted at a time: the server, each `--node` in turn, the server again.
All of it is one Bluetooth stream, so the PC's own latency cancels and each
line is that node's delay against the server's, plus the sound's flight to the
mic (~2.9 ms a metre). The two server phases must agree to a fraction of a
millisecond, or the reference moved and nothing between is worth quoting:

```
python tools/btlisten/sync.py --server COM20 --node COM9 --node COM22:-24 --level -6 --volume 120 --clicks 12
```

About 35 s, mostly silence, each node clicking for ~5 s. The MAX98357A nodes
need that level and volume to be heard over a quiet room; `:-24` keeps
WROVER2's TPA3116 from being deafening meanwhile. Every mute is read before it
is toggled and put back as found. Since D14 the clients play within 1.5 ms of
the server (`logs/sync-20260930-175830-synced-136ms`); before, they were 44 and
51 ms behind it. An `und=+1` on each client afterwards is the stream ending as
the recording stops, not a hole in the run.

**`soak.py` — what a long stream does.** The silent long run behind the
server: a quiet tone for `--secs` with every node muted, and every 10 s, while
the stream is still open, each client's telemetry and the server's `a` window:

```
python tools/btlisten/soak.py --server COM20 --node COM9 --node COM22 --secs 600 --trace
```

Per client: underruns (`und`), `dry`, schedule jumps (`sjmp`), the range of
the timing error (`se`), the blocks lost and rebuilt, the lost-run histogram
and `late` (copies that came after their silence played); for the server its
own `und`/`dry`/`ovf` and the worst gap between Bluetooth packets. Good is no
`und`, `dry` or `sjmp` anywhere, `se` within about ±1 ms, `late=0`. `lost` and
`rec` swing with how many frames the server loses that minute (3–12%), so
compare runs by `late`, `und` and `sjmp`, which do not.

**Which redundancy, scored on one recording.** Because the server's loss
swings 3× from minute to minute, two schemes run in alternating minutes are
not compared on the same losses. `--trace` turns on each client's loss trace
(`L1`: a bit per packet, 1 = never heard) and keeps it in the same log, and
`losstrace.py` replays the firmware's rebuild rules on it — copy at every
distance, XOR of blocks 1 and d back in the client's single pass, and the same
XOR as if a client kept every parity:

```
python tools/btlisten/losstrace.py logs/soak-<time>.log
```

It prints, per client, the holes each scheme would have left and their run
lengths, and with two clients the share of the loss both missed — frames the
server never sent. Whatever the server sends during the run does not change
the losses, so it does not matter which scheme was on; what the trace cannot
score is `t2`, which changes the airtime and so the losses. Check a trace
against the board before believing it: the `[LOSS]` histogram `soak.py` prints
at the end must match the trace's own `lost runs`.

`mon.py COM20` logs the `a` window every 2 s to `logs/a2dp-<time>.log` while
somebody plays real music — for "it crackled just then".

Traps, each of which cost a run:

- **Windows' default microphone path erases the tone.** Its noise suppression
  treats a steady sine as hum and gates the rest to exact zeros. `listen.py`
  opens the mic in WASAPI RAW mode for that reason. Exclusive mode is no better
  on this laptop: the Realtek driver delivers 65–82 k frames/s for a 48 k
  stream, i.e. repeated audio — clicks of its own.
- **A muted microphone records exact zeros**, which reads like a speaker that
  is not playing. A real room is never digital silence, so `listen.py` stops
  and says so.
- **The volume resets on every reconnect**, and a tone 40 dB quieter looks like
  90 dips/s of noise. The script warns below −55 dBFS at the mic; above
  `peak 0.99` it clipped. Lower `--level` rather than the volume.
- **"Connected" in Windows is not the audio profile.** After a reboot Windows
  reported the device connected while the node sat in DISCOVERY and the
  endpoint was missing. If `listen.py` says no device matches, check the
  node's status line.
- **Every flash or reboot drops the Bluetooth link** and it does not come back
  by itself (`set_auto_reconnect(false)`): `k`, or somebody clicks Connect.
- **Opening the serial port can reset the node** — and drop the link under
  test. `ser.py` sets DTR and RTS false *before* the port opens; `pio device
  monitor` does not.
- **Silence from one node with healthy counters is its DAC.** The stereo
  WROOM reported `rx`, `lost=28 und=0` as a CLIENT and played nothing: its
  PCM5102A's pins had never been soldered. A PCM5102A also stays silent with
  XSMT low or SCK floating (README, pin configuration).

## Updating without a cable: `tools/ota.ps1`

Update mode (D15) is tested with the target still on USB, so its log can be
read, and another node on USB as the relay. Store the WiFi on the target first
with `tools/ota-wifi.ps1` — a person types the password.

```powershell
./tools/ota.ps1 -Env esp32stereo -Relay COM20
```

The target's log should show, in order: `[OTA] update requested over the
mesh`, `[OTA] update mode ...`, `[OTA] ready ip=...`, `[OTA] receiving`,
`[OTA] written slot=appN`, a reboot into `[OTA] boot ... pending=1`, then on the
script's second request `[OTA] image kept (update requested)`. The script ends
with `PASS` only if the version *and* the slot changed.

**Rollback.** Build an image that crashes — `abort()` straight after the
`[OTA] boot` line in `setup()`, into a build directory of its own — revert the
source, and send it with `-NoBuild -NoVerify` and `PLATFORMIO_BUILD_DIR`
pointing at that directory. The log must show the crashing image boot with
`pending=1`, the abort, and the previous image back two seconds later with
`rolledback=` naming the slot it refused.

**Giving up.** `W` with an empty first line shows what is stored. With nothing
stored, bare `U` comes back in ~3 s with `reason=no_wifi`; with an SSID that
does not exist (`W<name>` and an empty password line), in ~35 s with
`reason=wifi`. Either way the node is back on the mesh by itself — and the
dummy SSID stays stored until `ota-wifi.ps1` replaces it.

**A node across the house.** It has no serial port, and a client never
transmits, so its reception is read with `-Status` while a stream plays at it:
`./tools/ota.ps1 -Env esp32stereo -Relay COM9 -Status` prints `rssi=` (the home
WiFi) and `before=CLIENT up= rx= lost= und= ... runs=` (the mesh, up to the
request). The relay can be any node on USB, a client of the same stream
included. Behind `bench-mesh.ps1`, keep the relay out of the harness (`-Ports`)
and ask before the harness stops the source: a node back in DISCOVERY has
already zeroed its counters.

## By hand, with a phone

What no script can do: a phone connecting, leaving, and moving to another node.
It needs a server-capable node (a WROVER; a WROOM, S3 or C3 cannot be one, D3),
a phone, and any other boards as clients, with a DAC on each one you want to
hear. Capture each board's log so two runs can be compared:

```
./tools/capture-serial.ps1 -Environment esp32wrover
```

### Step 1 — Boot

Flash and monitor each board on its own (`pio run -e esp32wrover -t upload`,
then `pio device monitor -e esp32wrover`). Expect:

```
  SonoLoco — Multi-Room Audio
  Firmware: v0.2.0-…
  Mode: SERVER capable (BT + ESP-NOW)
[INFO]  PSRAM: 4194304 bytes
[INFO]  Heap after WiFi init: ...
[INFO]  ESP-NOW ready — MAC: XX:XX:XX:XX:XX:XX
[INFO]  BT discoverable as: SonoLoco-WROVER
[INFO]  Setup complete. Entering DISCOVERY...
```

**Pass:** `PSRAM:` non-zero on a WROVER — at 0 the mesh is off by design (D3).
A WROOM prints `No PSRAM detected` unless it is in client-only mode (`c`),
where it prints `Mode: CLIENT only (configured — BT never starts)` and joins
the mesh. `Firmware:` has no trailing `*`. A power-on plays the startup sound.

### Step 2 — DISCOVERY → SERVER on phone connect

Connect the phone to the WROVER and play something.

**Expect:** `BT connected`, `=== SERVER — local playback + ESP-NOW broadcast
===`, `BT audio started → ESP-NOW TX will activate in …`.

**Pass:** audio from the server's DAC; the connect jingle does not distort the
music after it; status lines every 10 s with `mode=SERVER` and **`qfull=0
senderr=0 radiofail=0`** — any of those climbing means the radio cannot keep up
with ~387 packets/s.

### Step 3 — Every other board reaches CLIENT and holds

**Expect on each:**

```
[INFO]  === CLIENT — ESP-NOW → I2S ===
[INFO]  Stopping BT to release I2S...
[INFO]  I2S output at 44100 Hz stereo
[SYNC] armed late=…us dropped=… frames
[INFO]  Ring ready (… bytes) — starting I2S, on the server's schedule
```

**Pass:** it starts within a few seconds of the server streaming; audio from
its DAC; and in its status line `und=0 ovf=0`, `sync=1` and `se=` (µs from the
server's schedule) within about ±1 ms. `lost` is the holes left after
redundancy, `rec` the blocks it rebuilt.

**The rooms should sound as one.** Since D14 every node plays on the server's
schedule, measured within 1.5 ms. An echo between rooms is a bug — measure it
with `sync.py`.

### Step 4 — Five minutes

**Pass:** `ovf`, `und`, `dup`, `rsy` stay at 0; `sjmp` stays at 0; `heap=`
is flat (a slow decline is a leak). `lost` grows with the server's own frame
loss, and should stay well under 1% of `rx`.

### Step 5 — Teardown and recovery

The transitions that have historically broken:

1. **Disconnect the phone.** Every board prints `=== DISCOVERY ===`, a client
   after `ESP-NOW silent for 5s → DISCOVERY`.
2. **Reconnect the phone to the same node.** It must become SERVER again. This is
   what `a2dpSink.end(false)` exists for; if it fails here, the controller was
   freed.
3. **Connect the phone to *another* WROVER.** The roles must swap — the
   premise of the project, and the step most likely to expose a stale
   `senderLocked`, a node that ignores every future server. A node playing as
   a client cannot be connected to until it has fallen back to DISCOVERY
   (`TODO.md`).
4. **Reboot the server mid-stream.** A client should log `rsy=1` and keep
   playing, not report tens of thousands of lost packets.

**Pass:** all four, repeatedly, with no power cycle.

## Recording the result

Keep the logs, and record the firmware version beside every number in
`CHANGELOG.md`: a measurement whose build cannot be identified is an anecdote.
`bench-mesh.ps1` writes the version of every node into its log header; a manual
capture has none, so take it from the boot banner.
