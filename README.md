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
                                │
                     ┌──────────┴──────────┐
                     ▼                     ▼
                  Client 1             Client 2  ...
```

**CLIENT mode:**
```
ESP-NOW RX ──► jitter buffer (~185ms) ──► I2S ──► PCM5102 ──► TPA3116 ──► Speaker
```

The 4x reduction is 2x from halving the sample rate and 2x from stereo → mono. The
server plays the full 44.1 kHz stereo stream locally, so rooms do not currently sound
identical — see `TODO.md`.

## Current Status

The Bluetooth speaker half works on hardware: A2DP sink, I2S output to the
PCM5102, notification sounds, volume.

**The ESP-NOW mesh works**, as of 2026-08-19: a WROOM sourcing and an ESP32-S3
playing, 26,279 packets over 120 s with zero lost, overflowed, underrun,
duplicated or resynced. Run it yourself with `./tools/bench-mesh.ps1 -Flash`.

**The Bluetooth server path is still unproven** — audio taken from a phone and
forwarded to clients. No board on the bench can do it: the WROOM has no PSRAM and
the S3 has no BT Classic, so that needs a WROVER.

Also still open: the clocks drift (measured at roughly 25-45 ppm between these
two boards, enough to empty a client's jitter buffer in about half an hour) and
nothing corrects for it yet.

Where things are written down, so they stay in one place each:

| File | Owns |
|------|------|
| `TODO.md` | everything still open, including the known timing and bandwidth problems |
| `CHANGELOG.md` | what has already been done and when |
| `docs/decisions.md` | why the architecture is what it is, and what would change it |
| `docs/bench-test.md` | the two-board bring-up procedure and its pass criteria |

## Hardware

### ESP32 Module Compatibility

| Module | BT Classic | PSRAM | Role | Notes |
|--------|-----------|-------|------|-------|
| ESP32 WROOM 32 | Yes | No | BT speaker; client-capable | BT + WiFi together exhausts the heap, so the firmware disables ESP-NOW. With BT never started it does ESP-NOW fine — proven on the bench — but no runtime mode exposes that yet |
| ESP32 WROVER | Yes | Yes (4MB) | **Universal** | Recommended — runs full firmware, can be server or client |
| ESP32-S3 | No | Yes | Client only | Good CPU/RAM but no BT Classic; receives audio via ESP-NOW |
| ESP32-C6 | No (LE only) | No | Client only | WiFi 6 + Thread, but no BT Classic; weaker than S3 for this use |

The goal is for every node to be interchangeable. Only the **WROVER** currently meets that requirement.

**Why the WROOM can't do the mesh.** Running BT Classic and WiFi at the same time on
an ESP32 needs PSRAM. Without it, WiFi claims ~80 KB of the ~215 KB DRAM heap and the
BT stack can no longer allocate its L2CAP/AVDTP buffers, so it crashes. `setupESPNow()`
therefore *deliberately bails out* on a board that has BT compiled in and no PSRAM.
A WROOM node is a plain Bluetooth speaker with no mesh — that is by design, not a bug.

The S3 and C6 have the opposite problem: plenty of RAM, no BT Classic at all. They are
compiled without `-DENABLE_BLUETOOTH` and can only ever be clients.

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

### Build & Upload

There are **two builds**, because there are only two instruction sets — and three
environment names, because there are three boards with three COM ports:

| Environment   | Board        | Port | Build | Role |
|---------------|--------------|------|-------|------|
| `esp32dev`    | ESP32 WROOM  | COM7 | `esp32_classic` | Local BT playback only (no PSRAM → no mesh) |
| `esp32wrover` | ESP32 WROVER | COM9 | `esp32_classic` | SERVER or CLIENT — the reference node |
| `esp32s3`     | ESP32-S3     | COM8 | `esp32s3_client` | CLIENT only (no BT Classic) |

`esp32dev` and `esp32wrover` compile **the same binary**; they exist as separate names
only so each board keeps its port. The Arduino core ships `CONFIG_SPIRAM=y` with
`CONFIG_SPIRAM_BOOT_INIT` unset, so `psramInit()` probes for PSRAM at boot and only
logs a warning when there is none — one image boots on both modules and
`setupESPNow()` picks the role at runtime. That is the project premise, so resist
adding a third build; if a board needs different *behaviour*, detect it at runtime.

The S3 must stay separate: different architecture, no BT Classic, and the A2DP
library will not compile for it.

```bash
cd esp32-code

pio run -e esp32dev --target upload
pio device monitor -e esp32dev
pio device list                      # verify the ports — COM8/COM9 are guesses
```

PlatformIO is not on `PATH` on the dev machine; use the full path:

```bash
/c/Users/sebai/.platformio/penv/Scripts/pio.exe run -e esp32dev --target upload
```

### Tests

The ring buffer and packet sequence accounting run on the host, no board needed:

```bash
pio test -e native
```

This needs a host compiler (gcc/clang/MSVC), which is *not* currently installed
on the dev machine — see `TODO.md`. Until it is, the same tests run on a
connected board with `pio test -e esp32dev`, using the cross-toolchain
PlatformIO already has.

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
- `esp32-code/test/test_jitter/` — host tests for the above. Each one
  corresponds to a real bug or a real invariant.
- `tools/capture-serial.ps1` — timestamped serial capture, so two bench runs can
  be compared.

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
- **Don't name a global `btStarted`.** Arduino's `esp32-hal-bt.h` already declares
  `bool btStarted()` at global scope and the collision is a hard compile error.
- **Role state must be cleared on every entry to DISCOVERY,** not just when leaving
  CLIENT. A stale `senderLocked` makes a node ignore every future server forever.
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
