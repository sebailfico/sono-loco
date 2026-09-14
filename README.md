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
Phone ──BT A2DP──► ESP32 ──I2S──► PCM5102 ──► TPA3116 ──► Speaker
                     │
                     └── 4-tap FIR, then downsample 44.1kHz stereo → 22.05kHz mono
                                │
                           ESP-NOW broadcast (200-byte packets, ~220/sec, 44 KB/s)
                           stamped with this household's 16-bit mesh id
                                │
                     ┌──────────┴──────────┐
                     ▼                     ▼
                  Client 1             Client 2  ...
```

**CLIENT mode:**
```
ESP-NOW RX ──► jitter buffer ──► I2S DMA ──► PCM5102 ──► TPA3116 ──► Speaker
                (~91ms prefill)   (~46ms)
```

Client-side latency is about **137 ms**: playback starts once `JITTER_PREFILL`
(4000 bytes ≈ 91 ms) has accumulated, and the I2S DMA ring holds a further
1024 frames ≈ 46 ms. The ring itself is 8192 bytes ≈ 185 ms, which is its
capacity, not its latency — the prefill must stay above the DMA capacity, see
the gotchas.

The 4x reduction is 2x from halving the sample rate and 2x from stereo → mono. The
server plays the full 44.1 kHz stereo stream locally, so rooms do not currently sound
identical — see `TODO.md`.

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

**The Bluetooth server path carries audio, badly.** As of 2026-09-14 evening a
phone streams into a WROVER-E, which plays it locally and forwards it over
ESP-NOW — after a boot loop (`WIFI_PS_NONE` gotcha) and a crash on connect
(15 KB of internal DRAM was not enough for one L2CAP link; it is 43 KB now).
What comes out of the clients is crackly: with the BT radio streaming, a fifth
of the ESP-NOW frames never make it onto the air, and the client sees 24%
loss. That is BT/WiFi coexistence on one chip, it is measured, and it is the
first item in `TODO.md`.

**Clock drift is corrected**, as of 2026-08-20 (v0.2.0). The clocks do drift —
measured at −30.5 ppm between the WROOM and the S3, and −57.7 ppm between the
same WROOM and an ESP32-C3, enough to drain a client's jitter buffer to an
underrun within a 600 s run. `lib/drift/` holds the buffer at depth by
duplicating or dropping one mono sample at a time, roughly one edit a second at
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
| `esp32wrover` | ESP32 WROVER-E | COM12 | `esp32_classic` | SERVER or CLIENT — the reference node. Attached 2026-09-14 |
| `esp32wrover2` | ESP32 WROVER-E + DAC | COM13 | `esp32_classic` | Same binary, second name so a phone can tell the two apart |
| `esp32s3`     | ESP32-S3     | COM9 | `esp32s3_client` | CLIENT only (no BT Classic) |
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
/c/Users/sebai/.platformio/penv/Scripts/pio.exe run -e esp32dev --target upload
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
firmware, streams a synthetic 22.05 kHz tone between them and reports packet loss
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
- `esp32-code/lib/jitter/` — the client's ring buffer (`jitter.h`) and packet
  sequence accounting (`seqtracker.h`). Pure logic, no Arduino or ESP-IDF, so it
  can be tested on a PC. This is where both of the worst bugs in this project
  lived.
- `esp32-code/lib/mesh/` — the mesh name to mesh id derivation. Pure arithmetic
  on a string, and in a library because two nodes disagreeing about what a name
  hashes to produces silence with nothing in the log — the one failure mode
  worth pinning on the host rather than chasing on a bench. See D12.
- `esp32-code/lib/drift/` — the clock-drift controller. Decides when a client
  should duplicate or drop a sample to hold its buffer at depth; it never touches
  I2S or the ring buffer itself, which is what makes the closed loop simulable on
  a PC. See D11.
- `esp32-code/test/test_jitter/` — host tests for the ring buffer and sequence
  accounting. Each one corresponds to a real bug or a real invariant.
- `esp32-code/test/test_mesh/` — host tests for mesh identity, including known
  names pinned to known ids: changing the hash would split every deployed mesh
  silently, so it should take a failing test to do it.
- `esp32-code/test/test_drift/` — host tests for the controller, including
  hour-long closed-loop simulations at the drift measured on these boards. The
  model is checked against the recorded 1.34 B/s slope before anything built on
  it is believed.
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

Comments in the code explain *why*, particularly where a line looks wrong but isn't.

## Gotchas That Have Already Bitten This Code

Each of these was a real bug. Don't re-introduce them.

- **`i2s_write()` may accept fewer bytes than requested.** Always use `i2sWriteAll()`,
  or advance by the returned `bytes_written`. Ignoring it silently discards audio.
- **The jitter buffer must be pushed whole blocks or not at all.** Dropping an odd
  number of bytes shifts every later 16-bit sample by one byte and never re-aligns —
  a permanent noise stream, not a glitch. `JITTER_BUF_SIZE` must stay a power of two
  (a `static_assert` enforces it).
- **`a2dpSink.end(true)` frees the BT controller permanently.** The node can then never
  be a SERVER again until it is power-cycled. Use `end(false)`.
- **Never `Serial.print` from the ESP-NOW send/recv callbacks.** They fire ~220×/s and a
  blocking UART write there causes the very dropouts it would be reporting. Bump a
  counter, print from `loop()`.
- **`esp_now_send()` back-to-back** without waiting for the send callback returns
  `ESP_ERR_ESPNOW_NO_MEM` and drops silently. TX is gated on a semaphore.
- **Decimation must go through the 4-tap FIR.** Taking every other sample folds
  11–22 kHz straight back into the audible band.
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
  timestamps. `ESPNOW_PHY_RATE` sets 6 Mbps; measured, see `CHANGELOG.md`.
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
