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

### What Works
- [x] Bluetooth A2DP sink (phone → ESP32)
- [x] I2S output to PCM5102 DAC
- [x] Notification sounds (startup, connect, disconnect)
- [x] Basic volume control

### Written but never validated on hardware
The whole mesh path is implemented and compiles for all three environments, but no
part of it has been run on two boards yet. Treat it as unproven.
- [ ] ESP-NOW audio broadcast (server → clients)
- [ ] Jitter buffer + I2S playback on clients
- [ ] Runtime SERVER/CLIENT role negotiation

See `TODO.md` for the bench-test plan and the open design issues (clock drift,
server/client time alignment, assumed 44.1 kHz sample rate).

### Roadmap
- [ ] Fix EMI/interference issues
- [ ] ESPectre presence detection (auto-pause when room is empty)
- [ ] Dynamic volume based on presence
- [ ] Home Assistant integration
- [ ] OTA updates

## Hardware

### ESP32 Module Compatibility

| Module | BT Classic | PSRAM | Role | Notes |
|--------|-----------|-------|------|-------|
| ESP32 WROOM 32 | Yes | No | Client only | BT + WiFi simultaneously exhausts heap; ESP-NOW disabled at runtime |
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

Edit `esp32-code/include/config.h`:

```cpp
#define ROOM_NAME "LivingRoom"   // Visible as Bluetooth speaker name
#define ESPNOW_CHANNEL 1         // Must be the same on all nodes
```

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

## Code Layout

Two files, deliberately:

- `esp32-code/src/main.cpp` — everything: state machine, tones, ESP-NOW TX/RX,
  jitter buffer, A2DP callbacks, I2S setup.
- `esp32-code/include/config.h` — every tuneable number. New constants go here,
  never inline in `main.cpp`.

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

## Libraries

- [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) — Bluetooth A2DP Sink

## License

MIT License
