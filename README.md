# SonoLoco

DIY multi-room audio system using ESP32 microcontrollers. No WiFi router, no central server, no configuration.

## The Core Idea

**Every node is identical.** Flash the same firmware to every speaker. Connect your phone to any one of them — that node becomes the server, all others sync and play automatically. Disconnect your phone — everyone resets and waits for the next connection. Any node can be the server. Any node can be a client. Roles are negotiated at runtime.

```
  📱 Phone
      │  Bluetooth A2DP (connect to ANY node)
      ▼
  ┌──────────┐
  │  NODE A  │ ──── ESP-NOW broadcast ────► ┌──────────┐
  │ (SERVER) │                              │  NODE B  │
  │  plays   │ ──── ESP-NOW broadcast ────► │ (CLIENT) │
  └──────────┘                              │  plays   │
       │                                    └──────────┘
       └───── ESP-NOW broadcast ──────────► ┌──────────┐
                                            │  NODE C  │
                                            │ (CLIENT) │
                                            │  plays   │
                                            └──────────┘
```

Next time, connect to Node B. Node B becomes the server. Node A and C become clients. Same hardware, same firmware, different runtime role.

## State Machine

Every node runs the same three-state machine:

```
        ┌─────────────┐
        │  DISCOVERY  │  ← default on boot / after disconnect
        │  BT visible │
        │  ESP-NOW RX │
        └──────┬──────┘
               │
    ┌──────────┴──────────┐
    │ phone connects       │ hears ESP-NOW audio from another node
    ▼                      ▼
┌──────────┐         ┌──────────┐
│  SERVER  │         │  CLIENT  │
│ BT A2DP  │         │ ESP-NOW  │
│ plays    │         │ → plays  │
│ + sends  │         │          │
└──────────┘         └──────────┘
    │ phone disconnects    │ 5s of silence
    └──────────┬───────────┘
               ▼
        ┌─────────────┐
        │  DISCOVERY  │
        └─────────────┘
```

## Audio Path

**SERVER mode:**
```
Phone ──BT A2DP──► ESP32 ──► ring ──► I2S DMA ──► PCM5102 ──► TPA3116 ──► Speaker
                     │      (its own, exactly as a client's)
                     └── IMA ADPCM, 44.1kHz stereo, 4 bits a sample
                                │
                           ESP-NOW broadcast at 12 Mbps (248-byte packets,
                           ~387/sec: each a 2.6 ms block plus the one 11 back,
                           and when that block plays on the server's speaker)
                           stamped with this household's 16-bit mesh id
                                │
                     ┌──────────┴──────────┐
                     ▼                     ▼
                  Client 1             Client 2  ...
```

**CLIENT mode:**
```
ESP-NOW RX ──► ring ──► I2S DMA ──► PCM5102 ──► TPA3116 ──► Speaker
            (~47 ms)    (~44 ms)
```

**Every node plays each block at the same moment**, the server included
(D14, `lib/sync`). The server plays its own stream through its own ring and
DMA, exactly as a client does, about 90 ms after the A2DP packet reached it:
`JITTER_PREFILL` (16000 bytes of decoded stereo ≈ 91 ms) is what its ring
starts from. Each packet says when its block plays on the server's speaker,
and a client starts, then steers, onto that schedule, rather than onto a
depth of its own. The ring itself is 32768 bytes ≈ 185 ms, which is its
capacity, not its latency — the prefill must stay above the DMA capacity, see
the gotchas.

Before 2026-09-30 the server played through the A2DP library's own 46 ms I2S
ring, and a microphone put the clients 44–51 ms behind it: an echo between
any two rooms. `tools/btlisten/sync.py` is that measurement.

Clients get what the server plays: full-rate stereo, in a quarter of the bytes,
through IMA ADPCM (D5). Until 2026-09-29 the mesh carried 22.05 kHz mono PCM
instead — same bytes, no stereo, and nothing above ~11 kHz. A client with one
speaker mixes the two channels (`M`).

## Current Status

The Bluetooth speaker half works on hardware: A2DP sink, I2S output to the
PCM5102, notification sounds, volume.

**The ESP-NOW mesh works**, as of 2026-08-19: a WROOM sourcing and an ESP32-S3
playing, **132,069 packets over 600 s with zero lost, overflowed, underrun,
duplicated or resynced**. Run it yourself with `./tools/bench-mesh.ps1 -Flash`.
As of 2026-09-14 it works with **four clients at once** (two WROVERs, S3, C3):
zero loss on the S3 and C3 over 600 s on channel 11 while the house router was
busy on channel 1. On channel 1 the same boards had lost up to 10% — to the
router, not to each other; `tools/airmon/` is the sniffer that showed which.
What a building full of routers does to it is the open question, and the plan
for it is "Surviving the wild" in `TODO.md`.

**The Bluetooth server path works**, as of 2026-09-29: the PC streaming into
a WROVER-E, which plays it and forwards it, and a second WROVER playing the
mesh stream, **0.094% lost over 600 s**, and no holes a microphone could find in
short runs. Getting there took a boot loop (`WIFI_PS_NONE` gotcha), a crash on
connect (15 KB of internal DRAM was not enough for one L2CAP link; it is 43 KB
now), a 46 ms I2S ring for the server's own speaker, and one finding: a server
streaming Bluetooth loses 12–24% of its mesh frames to its own BT radio, one
frame at a time. Every frame then went out twice, at 12 Mbps instead of 6 (D13).
Since ADPCM each packet carries its own block and the one 11 packets back
instead: the server drops its frames in runs of 4–5 when its link has the
radio, and behind a streaming server, with a WROVER and an S3 both playing,
0.7% of blocks were still holes over 600 s (2026-09-30). That 0.094% was a client with
Bluetooth off; a Bluetooth-capable client kept scanning for a phone and lost
4.7% until it learned to switch its controller off. What is left — the
server's own ~3–9% — is the open problem in `TODO.md`. The PC can drive all
of it with nobody at the keyboard (`tools/btlisten/`, and `k` to reconnect a
reflashed server). A phone has not been tried on the new build, and range at
12 Mbps is untested; see `TODO.md`.

**Every node is meant to play each block at the same moment**, as of
2026-09-30 (D14) — coded and unit-tested on a board, not yet confirmed with
the microphone. Before it, clicks through the PC put the S3 51 ms and WROVER2
44 ms behind the server's own speaker: a clear echo between rooms. Now the
server plays through the same ring as its clients, every packet says when its
block plays on the server, and each client starts and steers on that.
`tools/btlisten/sync.py` measures it.

**Clock drift is corrected**, as of 2026-08-20 (v0.2.0). The clocks do drift —
measured at −30.5 ppm between the WROOM and the S3, and −57.7 ppm between the
same WROOM and an ESP32-C3, enough to drain a client's jitter buffer to an
underrun within a 600 s run. `lib/drift/` holds the buffer at depth by
duplicating or dropping one sample (a stereo frame since D5 changed) at a time, roughly one edit a second at
that offset. Measured over 600 s each way on the same boards: zero underruns
corrected, and the correction rate agrees with the uncorrected drift to within
1.4 ppm. See `CHANGELOG.md` and D11.

**Meshes are separated by a mesh id**, as of 2026-09-02: every packet carries a
16-bit id derived from a mesh name, and a client ignores anything that is not
its own, so two SonoLoco installations in radio range no longer join each
other's music. Name a mesh with `g<name>` over serial; move a node into one by
holding BOOT for three seconds at each end, with tones on the node saying what
happened. See D12 — and note none of it has been measured on hardware yet: two
meshes in the air, and the button itself, are both still unpressed.

Where things are written down, so they stay in one place each:

| File | Owns |
|------|------|
| `TODO.md` | everything still open, including the known timing and bandwidth problems |
| `CHANGELOG.md` | what has already been done and when |
| `docs/decisions.md` | why the architecture is what it is, and what would change it |
| `docs/bench-test.md` | how to test on hardware — the automated harness, and the manual walkthrough for the Bluetooth path |

## Hardware

### ESP32 Module Compatibility

| Module | BT Classic | PSRAM | Role | Notes |
|--------|-----------|-------|------|-------|
| ESP32 WROOM 32 | Yes | No | BT speaker **or** client | One or the other, chosen with `c` (client-only mode). BT + WiFi together exhausts the heap; with BT never started it runs the mesh fine — 12,870 packets in 60 s, no underruns |
| ESP32 WROVER | Yes | Yes (4MB) | **Universal** | Recommended — runs full firmware, can be server or client, both at once |
| ESP32-S3 | No | Yes | Client only | Good CPU/RAM but no BT Classic; receives audio via ESP-NOW |
| ESP32-C3 | No (LE only) | No | Client only | RISC-V, so its own build. Works as a client and as a bench source; measured -57.7 ppm against a WROOM |
| ESP32-C6 | No (LE only) | No | Client only | WiFi 6 + Thread, but no BT Classic; untried here |

The goal is for every node to be interchangeable. Only the **WROVER** meets that in
full — it is the only module that can be a server *and* switch to being a client.

**Why a WROOM has to choose.** Running BT Classic and WiFi at the same time on an
ESP32 needs PSRAM. Without it, WiFi claims ~80 KB of the ~215 KB DRAM heap and the BT
stack can no longer allocate its L2CAP/AVDTP buffers, so it crashes. `setupESPNow()`
therefore bails out on a board that has BT compiled in, no PSRAM, and Bluetooth about
to start.

The escape is to not start Bluetooth: a node in **client-only mode** (`c`) gets the
radio and joins the mesh, and a WROOM becomes a real node instead of a standalone
speaker. What it cannot be is a server, which genuinely does need both at once. See D3.

The S3, C3 and C6 have the opposite problem: plenty of RAM, no BT Classic at all. They
are compiled without `-DENABLE_BLUETOOTH` and can only ever be clients.

Server and clients share one radio between BT and ESP-NOW, so mesh bandwidth is tight —
see the bandwidth note in `TODO.md`.

### Bill of Materials (per node)

| Component | Model | Cost |
|-----------|-------|------|
| Microcontroller | ESP32 WROVER | ~€7 |
| DAC | PCM5102 I2S module | ~€3 |
| Amplifier | TPA3116 Class D 2x50W | ~€10 |
| Power Supply | 12V DC adapter | ~€10 |
| **Total** | | **~€30** |

### Wiring Diagram

```
┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐
│     ESP32       │      │    PCM5102      │      │    TPA3116      │
│                 │      │     (DAC)       │      │     (AMP)       │
│  GPIO26 (BCK)  ─┼──────┼─► BCK           │      │                 │
│  GPIO25 (DATA) ─┼──────┼─► DIN           │      │                 │
│  GPIO22 (WS)   ─┼──────┼─► LCK           │      │                 │
│  3.3V ──────────┼──────┼─► VCC           │      │                 │
│  GND ───────────┼──────┼─► GND           │      │                 │
│                 │      │  LOUT ──────────┼──────┼─► L IN          │
│                 │      │  ROUT ──────────┼──────┼─► R IN          │
│                 │      │  GND ───────────┼──────┼─► GND           │
└─────────────────┘      └─────────────────┘      │  L+/L- ──► 🔊 L │
                                                  │  R+/R- ──► 🔊 R │
                                                  │  12V DC ◄── PSU │
                                                  └─────────────────┘
```

### PCM5102 Pin Configuration

| Pin | Connection | Function |
|-----|------------|----------|
| FLT | GND | Normal latency |
| DEMP | GND | De-emphasis off |
| XSMT | 3.3V | Soft mute OFF |
| FMT | GND | I2S format |
| SCK | GND | Clock generated internally |

### Hardware Tips

**To reduce EMI/interference:**
- Keep I2S wires short (<10cm) and twisted together
- Route wires away from the ESP32 antenna (corner with metal shield)
- Add ferrite beads on I2S lines
- Use separate power supplies for ESP32 and amplifier
- Add decoupling capacitors (100nF + 10µF) near ESP32 and DAC

**If it turns harsh and distorted above some volume, check the amp's supply
first.** On 2026-09-28 a TPA3116 fed below its 12 V distorted from 50–60%
upwards, on two different speakers; at 12 V it was fine.

**To reduce volume with TPA3116:**
- Check for gain jumpers on your TPA3116 module (20dB/26dB/32dB)
- Add a resistor voltage divider between DAC and amp input

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)

### Configuration

Per-node settings live in `esp32-code/platformio.ini`, alongside that node's COM
port — so the source tree is identical for every board and you flash an
*environment*, not an edited file:

```ini
[env:esp32wrover]
extends = esp32_classic
build_flags =
    ${esp32_classic.build_flags}      ; extend, never replace — see the gotchas
    -DROOM_NAME='"LivingRoom"'        ; also the Bluetooth name, keep them distinct
```

Everything else is in `esp32-code/include/config.h` and is the same on every
node — most importantly `ESPNOW_CHANNEL`, which **must** match across the mesh.

The **mesh name** is the exception to both: it is per household rather than per
node or per build, so it lives in NVS on the board and `config.h` only supplies
the factory default (`MESH_NAME`). Every packet carries the 16-bit id it hashes
to, and a client ignores every packet that is not its own — which is what keeps
your neighbour's three speakers out of your stream.

Naming a mesh needs a console, so it is what the first server gets set up with:

```
g Casa Rossi     # over serial: name this mesh, stored in NVS
```

Moving a node into a mesh afterwards needs no console and no typing — it is a
**three-second hold of the BOOT button at each end**, and a node on a wall is
exactly the case that has to work:

1. Hold BOOT on the server whose mesh you are joining. It beeps twice and
   *offers* its mesh for 60 s.
2. Hold BOOT on the node you are moving. It beeps twice, adopts the offer, and
   plays the rising three-note tone when it has joined. A falling tone means the
   window closed with no offer heard — press again.

Both presses are required. A node that adopted whatever stream it happened to
hear could be captured by a neighbour who simply played music during the window;
an offer has to be made by somebody standing at the other mesh. The same two
halves are on the serial console as `o` (offer) and `p` (listen), which is how
you move a *server* into somebody else's mesh — the button on a server-capable
node always offers.

Nodes only hear each other if their ids match, and a mismatch looks exactly like
being out of range — `mesh=` on the identify line and `fgn=` (foreign packets
dropped) in the status line are how you tell the two apart. See D12 in
`docs/decisions.md`.

### Build & Upload

There is **one build per instruction set** — three of them — and one environment
name per board that might be plugged in:

| Environment   | Board        | Port | Build | Role |
|---------------|--------------|------|-------|------|
| `esp32dev`    | ESP32 WROOM  | COM8 | `esp32_classic` | BT speaker, **or** a mesh client in client-only mode (`c`). Not both: no PSRAM means BT and WiFi cannot run together |
| `esp32wrover` | ESP32 WROVER-E + MAX98357A | COM20 | `esp32_classic` | SERVER or CLIENT — the reference node. Attached 2026-09-14 (COM12, COM20, COM23 before) |
| `esp32wrover2` | ESP32 WROVER-E + PCM5102 + TPA3116 | COM22 | `esp32_classic` | Same binary, second name so a phone can tell the two apart. COM13, COM19, COM21, COM11 before: a CH340 is numbered by USB socket, so check the MAC |
| `esp32s3`     | ESP32-S3 + MAX98357A (since 2026-09-30) | COM9 | `esp32s3_client` | CLIENT only (no BT Classic). Plug its native USB port: on the CH343 port it flashes but prints nothing |
| `esp32c3`     | ESP32-C3     | COM10 | `esp32c3_client` | CLIENT only (no BT Classic). RISC-V, hence its own build |

Ports confirmed with `pio device list` and `esptool chip_id` (2026-08-19; the
WROVER on 2026-09-14).
`tools/bench-mesh.ps1` does not depend on them — it discovers ports and
identifies each chip at run time, so a new board needs no edit here.

`esp32dev` and `esp32wrover` compile **the same binary**; they exist as separate names
only so each board keeps its port. The Arduino core ships `CONFIG_SPIRAM=y` with
`CONFIG_SPIRAM_BOOT_INIT` unset, so `psramInit()` probes for PSRAM at boot and only
logs a warning when there is none — one image boots on both modules and
`setupESPNow()` picks the role at runtime. That is the project premise, so resist
adding a build config for anything but a new instruction set; if a board needs
different *behaviour*, detect it at runtime or make it a setting, the way
client-only mode is.

The S3 and C3 must stay separate: different architectures — Xtensa and RISC-V —
no BT Classic on either, and the A2DP library will not compile for them.

```bash
cd esp32-code

pio run -e esp32dev --target upload
pio device monitor -e esp32dev
pio device list                      # re-check the ports after plugging a board in
```

PlatformIO is not on `PATH` on the dev machine; use the full path:

```bash
pio run -e esp32dev --target upload
```

Every build stamps itself with `git describe --tags --always --dirty=*`, printed
by the build, in the boot banner and in the node's `?` identify line:

```
  SonoLoco — Multi-Room Audio
  Firmware: v0.1.0-3-gabc1234
```

A trailing `*` means the build came from a tree with uncommitted changes, so its
sha does not describe what is on the board. `tools/bench-mesh.ps1` warns about
that, and about a board running anything other than the tree you are reading —
which is the usual cause of a bench result that will not reproduce. See D10.

### Tests

The ring buffer and packet sequence accounting run on the host, no board needed:

```bash
pio test -e native
```

This needs a host compiler (gcc/clang/MSVC), which is *not* currently installed
on the dev machine — see `TODO.md`. Until it is, the same tests run on a
connected board with `pio test -e esp32dev`, using the cross-toolchain
PlatformIO already has.

To test the **mesh** — real boards, real radio:

```powershell
./tools/bench-mesh.ps1 -Flash -Duration 600
```

It discovers every attached ESP32, identifies each by chip, flashes the matching
firmware, streams a synthetic 44.1 kHz stereo tone between them and reports packet loss
and clock drift. No board limit. Full detail in `docs/bench-test.md`.

### Bench mode

Any node can be driven by hand over the serial monitor, in any build:

| Key | Effect |
|-----|--------|
| `?` | identify — firmware version, chip, PSRAM, MAC, whether BT and ESP-NOW are active |
| `b` | reboot into bench mode (Bluetooth stays off) |
| `n` | reboot into normal mode |
| `s` | start generating the synthetic test stream |
| `x` | stop generating it |
| `r` | print a telemetry line now |
| `d` | toggle clock-drift correction (on by default) |
| `c` | toggle client-only mode and reboot — the node then never starts Bluetooth, which is what lets a WROOM be a mesh client. Kept in NVS, so it survives a power cut |
| `g` | print the mesh identity; `g<name>` sets it. Kept in NVS, takes effect at once — no reboot, because nothing about the id is decided at boot |
| `p` | listen for 60 s and join the mesh that offers itself — the speaker half of pairing. Same as a three-second BOOT hold on a node that cannot be a server |
| `o` | offer this mesh for 60 s, so a listening node can join it — the server half. Same as a three-second BOOT hold on a server-capable node |
| `a` | BT server: print and reset the A2DP window — packets/s, packet size, a histogram of the gaps between packets from the Bluetooth stack, and the server's own ring (`jit`, `und`, `ovf`, `dry`). A gap longer than the ring holds is a hole in every room |
| `f` | BT server: toggle forwarding to the mesh. Local playback carries on, so one Bluetooth session can be measured with and without the mesh's transmissions |
| `w` | BT server: stop WiFi until the next reboot — the WROOM case, on a WROVER |
| `j` | BT server: play the connect jingle now, the way a connection does — into the server's own output, from `loop()`, so it cannot interleave with the stream; the ring re-arms afterwards and the clients follow |
| `k` | BT server: `k<aa:bb:cc:dd:ee:ff>` dials a bonded A2DP source, the way a headset reconnects to a phone. What lets a reflashed server get its link back with nobody clicking Connect; the PC here is `aa:bb:cc:dd:ee:ff` |
| `V` | BT server: `V<0..127>` sets the A2DP volume, as a phone's slider would. Applied before forwarding, so it moves every room; a server that dialled in with `k` starts at 1 |
| `m` | mute this node's speaker until reboot — zeroes what its ring hands to I2S, on a server and a client alike; the mesh and every timing are untouched. How a mic hears one node alone, and how `bench-mesh.ps1 -Mute` runs silent |
| `M` | client: mix stereo to mono on both channels, for a node with one speaker (a MAX98357A plays one channel). Kept in NVS |
| `e` | BT server: `e<n>` coexistence preference, 0 WiFi, 1 Bluetooth, 2 balance (default). Made no measurable difference |
| `t` | `t<n>` sends each mesh frame n times (1–3) until reboot; `ESPNOW_TX_COPIES` is the default |
| `D` | `D<n>` each packet carries the block n packets back as well (0–15, 0 = none) until reboot; on the wire, so clients follow. `MESH_REDUNDANCY_DISTANCE` is the default |
| `P` | `P<us>` spaces audio packets at least that far apart (0 = send each as soon as the radio is free) until reboot; `ESPNOW_TX_PACE_US` is the default |
| `R` | `R<Mbps>` sets the ESP-NOW PHY rate this node sends at, until reboot: 1, 2, 6…54 |
| `l` | client: print and reset the histogram of lost-run lengths (1..7, 8+) and `rec`, the blocks rebuilt from a later packet or a repeat copy |

Bench mode exists because the normal SERVER role needs a phone to connect over
A2DP, which cannot be automated. Because it never starts Bluetooth, it also runs
on a board with no PSRAM — which is how a WROOM can be tested on the mesh at all.
See D9 in `docs/decisions.md`.

## Code Layout

Deliberately small. `main.cpp` is one file on purpose — resist splitting it
further; the exception below is argued in `docs/decisions.md` (D7).

- `esp32-code/src/main.cpp` — the state machine, tones, ESP-NOW TX/RX, A2DP
  callbacks and I2S setup. Everything that needs real hardware.
- `esp32-code/include/config.h` — every tuneable number. New constants go here,
  never inline in `main.cpp`. Per-node values go in `platformio.ini` instead.
- `esp32-code/lib/jitter/` — the client's ring buffer (`jitter.h`), packet
  sequence accounting (`seqtracker.h`), and where each lost block's silence
  went so a later packet can patch it (`holes.h`). Pure logic, no Arduino or
  ESP-IDF, so it can be tested on a PC. This is where both of the worst bugs in this project
  lived.
- `esp32-code/lib/mesh/` — the mesh name to mesh id derivation. Pure arithmetic
  on a string, and in a library because two nodes disagreeing about what a name
  hashes to produces silence with nothing in the log — the one failure mode
  worth pinning on the host rather than chasing on a bench. See D12.
- `esp32-code/lib/adpcm/` — the mesh codec: IMA ADPCM, stereo, in blocks that
  each carry their decoder state, so any block decodes alone. Pinned to the
  Python reference in `tools/codec/abtest.py` by golden vectors: two nodes built
  from codecs that disagree decode each other into noise. See D5.
- `esp32-code/lib/drift/` — the clock-drift controller. Decides when a client
  should duplicate or drop a sample to hold its buffer at depth; it never touches
  I2S or the ring buffer itself, which is what makes the closed loop simulable on
  a PC. See D11.
- `esp32-code/lib/sync/` — playing in time: a node's output clock, read from
  its own blocking DMA writes, and the server's schedule as a client sees it
  through the packets' `due` stamps, earliest of each window. The arithmetic
  behind D14, where an error is a node playing cleanly a few ms away from the
  others — which nothing but a microphone would notice.
- `esp32-code/test/test_jitter/` — host tests for the ring buffer and sequence
  accounting. Each one corresponds to a real bug or a real invariant.
- `esp32-code/test/test_adpcm/` — the codec against the reference's own output,
  byte for byte, including the block layout that is the wire format.
- `esp32-code/test/test_mesh/` — host tests for mesh identity, including known
  names pinned to known ids: changing the hash would split every deployed mesh
  silently, so it should take a failing test to do it.
- `esp32-code/test/test_drift/` — host tests for the controller, including
  hour-long closed-loop simulations at the drift measured on these boards. The
  model is checked against the recorded 1.34 B/s slope before anything built on
  it is believed.
- `esp32-code/test/test_sync/` — host tests for `lib/sync`: wake latency, an
  output slower than nominal, a window of slow packets, a server clock with
  its own slope, a schedule that moves, and the 32-bit wrap in every one.
- `esp32-code/scripts/version.py` — a PlatformIO pre-build step that defines
  `FW_VERSION` from `git describe`. There is no version constant to bump by hand;
  see D10.
- `tools/airmon/` — a standalone sniffer for a spare classic ESP32: what is on
  the channel, second by second, and a 13-channel survey. `capture.py` logs it,
  `correlate.py` lines it up with a bench log.
- `tools/bench-mesh.ps1` — the automated multi-board mesh test: discover, flash,
  stream, measure drift, report. Scales to any number of boards.
- `tools/test-client-only.ps1` — checks that a BT-capable board really works as a
  mesh client on a *normal* boot. `bench-mesh.ps1` cannot cover this, because it
  puts every node into bench mode by design.
- `tools/capture-serial.ps1` — timestamped serial capture of a single node, so
  two manual runs can be compared.
- `tools/codec/abtest.py` — hear what a client plays before it is firmware: a
  WAV in, the original, the old 22.05 kHz mono path and the ADPCM path out, at
  the same rate and level. It decided D5, and its encoder is the reference
  `lib/adpcm` is tested against.
- `tools/btlisten/` — the Bluetooth half's test signal. The PC streams a 997 Hz
  tone to a server and records it with its own microphone, then counts holes,
  clicks and pitch error; with `--serial` the board's `a` window for the same
  seconds is printed beside it. `mon.py` logs that window every 2 s during
  ordinary use. `sync.py` measures how far apart the nodes play: clicks
  through the server, one node unmuted at a time.

Comments in the code explain *why*, particularly where a line looks wrong but isn't.

## Gotchas That Have Already Bitten This Code

Each of these was a real bug. Don't re-introduce them.

- **`i2s_write()` may accept fewer bytes than requested.** Always use `i2sWriteAll()`,
  or advance by the returned `bytes_written`. Ignoring it silently discards audio.
- **The jitter buffer must be pushed whole blocks or not at all, and moved in
  whole stereo frames.** Dropping an odd number of bytes shifts every later 16-bit
  sample by one byte and never re-aligns — a permanent noise stream, not a glitch.
  Moving by two bytes instead of four swaps left and right for the rest of the
  stream. Every push, advance and drift correction is a multiple of
  `CLIENT_FRAME_BYTES`. `JITTER_BUF_SIZE` must stay a power of two (a
  `static_assert` enforces it).
- **`a2dpSink.end(true)` frees the BT controller permanently.** The node can then never
  be a SERVER again until it is power-cycled. Use `end(false)`.
- **…but `end(false)` alone leaves Bluetooth running.** It deinitialises A2DP and
  AVRCP only: Bluedroid and the controller stay up, still page- and
  inquiry-scanning like a speaker waiting for a phone, and coexistence hands
  those scans the radio. A WROVER client lost 4.7% of the mesh, in runs of 8+,
  next to an S3 losing 2.0%. `stopBluetooth()` disables Bluedroid and the
  controller after it, keeping the controller's memory.
- **Disable Bluedroid, never deinit it, behind the A2DP library's back.** The
  library remembers having initialised it and skips `esp_bluedroid_init()` on
  the next `start()`, which then loops forever on "Failed to enable bluedroid"
  — the node is wedged and never becomes a speaker again.
- **Mute by zeroing samples, not by switching an output off.** When the A2DP
  library still wrote I2S itself, it installed its driver in `start()` and
  uninstalled it in `end()` only while `set_stream_reader(cb, true)`. `m`
  once turned that off, so a muted node kept the driver, and its next CLIENT
  install failed with `ESP_ERR_INVALID_STATE` every 5 s: stuck in DISCOVERY,
  counting the server's packets and playing none. The library's output is
  now off for good (D14) and `m` zeroes what the ring hands to I2S, on every
  node — the ring, the mesh and the schedule never notice.
- **Every write to the output driver must be counted in `outFrames`, tones
  included.** The output clock (D14) reads the DMA position as that count
  modulo the buffer length; a write that bypasses it puts every later
  reading off by up to a buffer (5.8 ms), in a way no counter shows.
  `outWrite()` and `i2sWriteAll()` both count.
- **The ring's frame numbering must advance for every frame that enters the
  ring, silence included, and for nothing else.** `ringPushed` is what a
  packet's `due` and a client's timeline are expressed in. A push that fails,
  or a gap longer than the silence it was given, leaves the numbering behind
  the stream, and the timeline is flushed (`timelineFlush`) rather than
  steered by.
- **Never `Serial.print` from the ESP-NOW send/recv callbacks.** They fire ~390×/s and a
  blocking UART write there causes the very dropouts it would be reporting. Bump a
  counter, print from `loop()`.
- **`esp_now_send()` back-to-back** without waiting for the send callback returns
  `ESP_ERR_ESPNOW_NO_MEM` and drops silently. TX is gated on a semaphore.
- **A node on an older firmware must not hear the new stream.** Before ADPCM a
  client pushed any payload straight into its buffer as PCM; fed ADPCM, that is
  full-scale noise through the amp. The mesh id on the wire is XORed with
  `MESH_WIRE_FORMAT`, so a node on the other format drops these packets as a
  foreign mesh (`fgn=` climbing). Change it whenever the payload changes
  meaning. (The old path's own trap — decimation that skips the FIR folds
  11–22 kHz into the audible band — went with the decimation.)
- **Sequence-gap arithmetic is unsigned.** A duplicate or reordered packet computes as
  a gap of ~65535; it has to be treated as a resync, not as 65535 lost packets.
- **Elapsed-time comparisons against a timestamp another task writes must be signed.**
  `millis() - lastRxMs` is unsigned, so a timestamp written one millisecond *after*
  this task read `millis()` wraps the difference to ~4.29 billion and every threshold
  test passes. That is not hypothetical: a client dropped to DISCOVERY announcing five
  seconds of ESP-NOW silence while its own receive counter was advancing by 221 packets
  a second (`silence now=13822 last=13823 age=4294967295`). Cast to `long` —
  `(long)(millis() - then) > TIMEOUT` — as `clientRetryAfterMs` already does.
  Comparisons against `loop()`'s own bookkeeping are safe, because only one task
  writes them.
- **Don't name a global `btStarted`.** Arduino's `esp32-hal-bt.h` already declares
  `bool btStarted()` at global scope and the collision is a hard compile error.
- **Role state must be cleared on every entry to DISCOVERY,** not just when leaving
  CLIENT. A stale `senderLocked` makes a node ignore every future server forever.
- **The mesh id must be checked before the sender lock, not after.** A client
  locks onto the first node it hears; if a neighbour's packet reaches that lock
  before the id comparison, the node pins itself to a mesh it will then ignore
  every packet from, and stays deaf to its own household until it next falls
  back to DISCOVERY. Same shape as the stale-`senderLocked` bug above.
- **A pairing beacon must be filtered out before the sequence tracker.** A
  beacon is an audio packet with no payload and `MESH_BEACON_SEQ` in the
  sequence field; let one reach `SeqTracker` and it computes a gap of tens of
  thousands, charges a resync and re-arms the jitter buffer — an audible
  interruption caused by a node that was only saying hello.
- **Pairing is a long press while running, never a press held through a reset.**
  BOOT is a strapping pin: held low across a reset it puts the chip into the ROM
  download mode, where no firmware runs at all and nothing can react to the
  button. (It is GPIO 9 on a C3 devkit and GPIO 0 on the others.)
- **Never put two `build_flags` keys in one `platformio.ini` section.** Duplicate keys
  in a single INI section are a hard `DuplicateOptionError` — the whole project stops
  loading, not just that environment. Extend a base section instead.
- **`JITTER_PREFILL` must exceed what the I2S DMA ring can swallow in one pass.**
  At 2000 bytes against a 4096-byte DMA ring, every arming of the jitter buffer
  was drained instantly and the client underran 24 times a second forever, while
  the audio limped along on DMA buffering alone. `static_assert`s tie the two
  constants together now.
- **The ESP32-S3 needs `-DARDUINO_USB_CDC_ON_BOOT=1`.** Its board definition sets
  `ARDUINO_USB_MODE=1` but leaves CDC off, so `Serial` goes to GPIO43/44 while
  the board enumerates on native USB — completely silent over the cable you are
  plugged into.
- **…and then `Serial` can stall the audio.** Native USB serial waits up to
  100 ms per write for a host that is plugged in but not reading, and `loop()`
  feeds I2S: an S3 on a PC with its port closed overflowed 9 times a minute.
  `setup()` gives it a 4 KB buffer and a 5 ms timeout — never 0, which the core
  turns into forever.
- **Bench mode's flag must be `RTC_NOINIT_ATTR`, not `RTC_DATA_ATTR`.**
  `.rtc.data` is re-initialised from the image on every boot that runs the
  bootloader, so the flag reads back as zero and the node reboots into normal
  mode instead.
- **ESP-NOW broadcasts at 1 Mbps unless told otherwise, and at 220 packets/s
  that is half the channel.** The default rate for ESP-NOW frames is 1 Mbps
  DSSS with a long preamble; a 206-byte frame is ~2.2 ms of air. With
  anything else on channel 1 the weakest receiver loses one packet in ten and
  the strongest loses none, in bursts every board sees at the same moment,
  which looks exactly like "clients degrade each other" until you check the
  timestamps. `ESPNOW_PHY_RATE` sets 12 Mbps; measured, see D13.
- **A node streaming Bluetooth loses mesh frames to its own radio, and the
  send callback calls every one a success.** 12–24% of broadcasts, one frame at
  a time, with `senderr=0 radiofail=0` on the server and nothing else on the
  channel. Only a receiver's `lost` shows it, and every client loses the same
  packets. That is why every block goes out twice, the second time 11 packets
  later (D13), past the 4–5-packet runs the server drops — and why "the server
  reports it sent them" proves nothing.
- **`len` is three fields: mask it with `ESPNOW_LEN_BYTES` before using it as
  a length.** The low byte is the payload length, bits 8–14 the redundancy
  distance, bit 15 the repeat flag. The first build that set the repeat bit
  also sent it: `ESPNOW_HEADER_SIZE + pkt.len` asked the radio for a 33 KB
  frame. The receiver splits it as it reads the header, before anything else
  looks.
- **A node that runs Bluetooth cannot turn WiFi power save off.** The IDF
  coexistence layer requires modem sleep while the BT controller is enabled and
  enforces it with `abort()`: `esp_wifi_set_ps(WIFI_PS_NONE)` before BT starts
  dies in `coex_core_enable`, after BT starts it dies in `pm_set_sleep_type`
  from the WiFi task. Either way a boot loop with no message but a backtrace.
  Found on the first boot of the first WROVER — the WROOM never reached this
  code because it bails before WiFi, and the S3/C3 have no BT. `setupESPNow()`
  now only sets `WIFI_PS_NONE` on a boot that will never start Bluetooth.
- **A `build_flags` in an `[env:...]` section replaces the parent's, it does not
  add to it.** Writing `build_flags = -DROOM_NAME='"Kitchen"'` under
  `extends = esp32_classic` silently drops `-DENABLE_BLUETOOTH` and the node
  quietly builds as a client. Always start the list with
  `${esp32_classic.build_flags}`. This one fails silently, unlike the duplicate
  key above — which makes it worse.

## Libraries

- [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) — Bluetooth A2DP Sink

## License

MIT License
