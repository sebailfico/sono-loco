# SonoLoco

DIY multi-room audio with ESP32 boards. No WiFi router, no central server, no configuration.

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
            (~136 ms, with the DMA)
```

**Every node plays each block at the same moment**, the server included
(D14). The server plays its own stream through its own ring and DMA, exactly
as a client does, about 136 ms after the A2DP packet reached it. Each packet
says when its block plays on the server's speaker, and a client starts, then
steers, onto that schedule.

Clients get what the server plays: 44.1 kHz stereo, in a quarter of the bytes,
through IMA ADPCM (D5). A client with one speaker mixes the two channels (`M`).

## Status

**It works, by ear and by counter.** A phone or the PC streams into a WROVER,
which plays and forwards; every other node plays the mesh — WROVERs, the S3,
the C3, and a WROOM in client-only mode at a stereo across the room.

- **In time:** every room within 1.5 ms of the server, by microphone (D14).
- **Clock drift corrected** (D11), by duplicating or dropping one frame at a time.
- **A clean mesh:** 600 s with nothing lost, overflowed or underrun
  (`./tools/bench-mesh.ps1 -Mute`).
- **No cable after the first flash:** updates come over the home WiFi (D15),
  and every command reaches every node over the mesh (D16).
- **Households apart** (D12): every packet carries a mesh id.

What is still open — the server's own radio losing mesh frames to Bluetooth,
two meshes side by side, range — is in `TODO.md`.

Where things are written down, one place each:

| File | Owns |
|------|------|
| `README.md` | what SonoLoco is, its hardware, and how to build, flash and drive it |
| `docs/decisions.md` | why the architecture is what it is, and what would change it |
| `docs/code-layout.md` | where each part of the code lives, and why there |
| `docs/gotchas.md` | bugs that already happened once — read before changing the firmware |
| `docs/bench-test.md` | testing on hardware: the automated harness, then the Bluetooth path by hand |
| `TODO.md` | everything still open |
| `CHANGELOG.md` | what has been done, newest first, with what it measured |
| `boards.local.md` | gitignored: this bench's MACs, Bluetooth addresses and volume trims |

## Hardware

### ESP32 Modules

| Module | BT Classic | PSRAM | Role | Notes |
|--------|-----------|-------|------|-------|
| ESP32 WROVER | Yes | Yes (4 MB) | **Universal** | Recommended — server or client, and switches between them |
| ESP32 WROOM 32 | Yes | No | BT speaker **or** client | One or the other, chosen with `c` (client-only mode) |
| ESP32-S3 | No | Yes | Client only | No BT Classic; receives the mesh |
| ESP32-C3 | No (LE only) | No | Client only | RISC-V, so its own build |
| ESP32-C6 | No (LE only) | No | Client only | Untried |

Only the **WROVER** makes every node interchangeable: it is the one module that
can be a server *and* switch to being a client.

**A WROOM has to choose.** Bluetooth Classic and WiFi together need PSRAM;
without it the Bluetooth stack runs out of RAM, so a WROOM about to start
Bluetooth leaves the mesh off. In **client-only mode** (`c`, kept in NVS) it
never starts Bluetooth and plays the mesh instead — the WROOM at the stereo
does. See D3. The S3, C3 and C6 have no Bluetooth Classic at all and can only
ever be clients.

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

Those are the classic ESP32's pins. The **S3 and C3** use GPIO 4 (BCK), 5 (WS)
and 6 (DATA), clear of their strapping pins (`config.h`). On a C3, ground the
DAC on a **G** pin, never on GPIO 9: that is BOOT, and held low it keeps the
board in download mode with no firmware running.

### PCM5102 Pin Configuration

| Pin | Connection | Function |
|-----|------------|----------|
| FLT | GND | Normal latency |
| DEMP | GND | De-emphasis off |
| XSMT | 3.3V | Soft mute OFF |
| FMT | GND | I2S format |
| SCK | GND | Clock generated internally |

All five must be tied as shown: with XSMT low the DAC soft-mutes, and with SCK
floating it never locks. A silent node whose counters look healthy is its DAC.

### Hardware Tips

**To reduce EMI/interference:**
- Keep I2S wires short (<10cm) and twisted together
- Route wires away from the ESP32 antenna (corner with metal shield)
- Add ferrite beads on I2S lines
- Use separate power supplies for ESP32 and amplifier
- Add decoupling capacitors (100nF + 10µF) near ESP32 and DAC

**To reduce volume with TPA3116:**
- Find the amount by ear first, with the node's own trim (`v-6`, `v-12`, …),
  then make it permanent in hardware and set the trim back to `v0`
- A resistor divider between DAC and amp input, per channel: 10 kΩ in series
  and 3.3 kΩ to ground is about −12 dB (4.7 k / 4.7 k −6 dB, 10 k / 1.5 k
  −18 dB). On the blue 2×50 W board it goes at the 3-pin input header
- Or the module's gain setting (the chip offers 20/26/32/36 dB), if your board
  exposes it — the blue 2×50 W board has no jumper for it

A MAX98357A is the other way round: at its 9 dB default gain it is quiet, so
start its trim at `v0`.

## Software

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI). On the dev
  machine it is not on `PATH`: run it as `~/.platformio/penv/Scripts/pio.exe`.

### Configuration

Per-node settings live in `esp32-code/platformio.ini`, alongside that node's COM
port — so the source tree is identical for every board and you flash an
*environment*, not an edited file:

```ini
[env:esp32wrover]
extends = esp32_classic
build_flags =
    ${esp32_classic.build_flags}      ; extend, never replace — see docs/gotchas.md
    -DROOM_NAME='"LivingRoom"'        ; also the Bluetooth name, keep them distinct
```

Everything else is in `esp32-code/include/config.h` and is the same on every
node — most importantly `ESPNOW_CHANNEL`, which **must** match across the mesh.

The **mesh name** is per household, so it lives in NVS on the board, and
`config.h` only supplies the factory default (`MESH_NAME`). Every packet carries
the 16-bit id it hashes to, and a client ignores every packet that is not its
own — which is what keeps your neighbour's three speakers out of your stream.
Name it with `g Casa Rossi` on the first node.

Moving a node into a mesh afterwards needs no console and no typing — it is a
**three-second hold of the BOOT button at each end**:

1. Hold BOOT on the server whose mesh you are joining. It beeps twice and
   *offers* its mesh for 60 s.
2. Hold BOOT on the node you are moving. It beeps twice, adopts the offer, and
   plays the rising three-note tone when it has joined. A falling tone means the
   window closed with no offer heard — press again.

Both presses are required, so a neighbour playing music during the window
cannot capture the node. On the console the two halves are `o` (offer) and `p`
(listen). Nodes only hear each other if their ids match, and a mismatch looks
exactly like being out of range — `mesh=` on the identify line and `fgn=`
(foreign packets dropped) in the status line tell the two apart. See D12.

### Build & Upload

There is **one build per instruction set** — three of them — and one environment
name per board that might be plugged in:

| Environment   | Board        | Port | Build | Role |
|---------------|--------------|------|-------|------|
| `esp32dev`    | ESP32 WROOM  | COM8 | `esp32_classic` | BT speaker, **or** a mesh client in client-only mode (`c`) |
| `esp32stereo` | ESP32 WROOM + PCM5102A | first flash only | `esp32_classic` | Client-only, at the stereo with no cable: updated over WiFi (`ota.ps1 -Env esp32stereo`) |
| `esp32wrover` | ESP32 WROVER-E + MAX98357A | COM20 | `esp32_classic` | SERVER or CLIENT — the reference node |
| `esp32wrover2` | ESP32 WROVER-E + PCM5102 + TPA3116 | COM22 | `esp32_classic` | Same binary, second name so a phone can tell the two apart |
| `esp32s3`     | ESP32-S3 + MAX98357A | COM9 | `esp32s3_client` | CLIENT only. Plug its native USB port: on the CH343 one it flashes but prints nothing |
| `esp32c3`     | ESP32-C3     | COM10 | `esp32c3_client` | CLIENT only. RISC-V, hence its own build |

```bash
cd esp32-code
pio run -e esp32wrover --target upload
pio device monitor -p COM20 --dtr 0 --rts 0 --eol LF --echo
pio device list                      # re-check the ports after plugging a board in
```

A CH340 is numbered by USB socket, so the WROVERs' ports move with every
replug: check the MAC that `?` prints. The harness and the tools discover the
ports themselves. Each board's MAC is in `boards.local.md`; anywhere tracked,
the placeholder is `aa:bb:cc:dd:ee:ff`, because the repo is public.

`--dtr 0 --rts 0` on the monitor matters: on a classic board those lines are
wired to reset and BOOT, and the C3's and S3's own USB port drives the same two
from them.

`esp32dev` and `esp32wrover` compile **the same binary**; one image boots on a
WROOM and a WROVER alike and picks its role at runtime. That is the project
premise, so add a build config only for a new instruction set; a board that
needs different *behaviour* gets a runtime check or a setting (D4).

Every build is stamped with `git describe --tags --always --dirty=*`, printed
in the boot banner and on the `?` line. A trailing `*` means uncommitted changes:
its sha does not describe what is on the board, and a bench result from it will
not reproduce. See D10.

### Tests

The pure logic in `lib/` — five suites, `test_jitter`, `test_adpcm`,
`test_mesh`, `test_drift` and `test_sync` — runs on the host, no board needed:

```bash
pio test -e native
```

That needs a host compiler, which is *not* installed on the dev machine yet
(`TODO.md`). Until it is, the same tests run on any board that is plugged in:
`pio test -e esp32wrover2 -f test_jitter`. The board is left running the test
image, so reflash it afterwards.

To test the **mesh** — real boards, real radio, silent:

```powershell
./tools/bench-mesh.ps1 -Flash -Duration 600 -Mute
```

It discovers every attached ESP32, flashes the matching firmware, streams a
synthetic 44.1 kHz stereo tone between them and reports packet loss and clock
drift. `-Mute` silences the boards on USB, not a node elsewhere in the same
mesh: give the bench a mesh of its own (`g`) when one is playing into a room.
Full detail, and the Bluetooth half, in `docs/bench-test.md`.

## Driving the Nodes

Every node takes commands: a letter, sometimes with an argument, typed on its
serial port. The same command runs on **any other node of the mesh** with
`@<target>` in front, typed on whichever node is on USB — so after the first
flash, a node needs no cable at all.

### Over the mesh

```
@SonoLoco-Stereo v-20     set the stereo's volume trim, from the desk
@* N                      every node: its mode, its source, what hearing it costs
@* v                      every node's trim
@SonoLoco-C3 r            one node's full status line
```

The target is a room name, a MAC, or `*` for every node. Each answer comes back
as `[@<name>] <line>`, then `[CMD] done replies=<n>`. A node that does not
answer is not on the mesh, not in this one, or on firmware from before D16.

`tools/mesh.ps1` does the same from the PC, through whichever node is on USB.
With no arguments it draws the mesh — each source, a server's phone or a bench
tone, with the clients locked to it and what each one loses:

```powershell
./tools/mesh.ps1
./tools/mesh.ps1 -Command v
./tools/mesh.ps1 -Target SonoLoco-Stereo -Command v-20
```

Three limits, on purpose. `*` refuses anything that reboots a node or takes it
off the mesh — `U b n c g p o w` — which would silence the house from one
keystroke. `W` never travels, because the password would be broadcast. A node
serving a phone refuses a remote `U`. See D16.

### Updates over WiFi

A node at a stereo across the house has power and nothing else. It takes a new
image over the home WiFi, with any node on USB relaying the request:

```powershell
./tools/ota-wifi.ps1 -Port COM22           # once per node, over USB: you type the WiFi password
./tools/ota.ps1 -Env esp32stereo           # from then on, from anywhere on the LAN
./tools/ota.ps1 -Env esp32stereo -Status   # no upload: its WiFi signal, and its stream counters
```

`ota.ps1` builds the image, has the relay broadcast `U<room name>`, finds the
node on the LAN when it reboots into **update mode**, uploads, and reads the new
version back. All of it is silent, and the script's output is the report. If
update mode gives up — no WiFi stored, a wrong password, five minutes with no
upload — the node goes back on the mesh by itself.

A new image is on probation until it has run a minute with its radio up; any
reset before that boots the previous one, so an update that breaks the mesh
undoes itself. A node needs one USB flash of a build from D15 onwards, for the
two-slot partition table, and its WiFi stored. The request is heard only inside
its own mesh. See D15.

### Commands

The same on every node and in every build; a few only mean something on a
Bluetooth server. `?` prints the node's state, so read it before toggling
anything (`m`, `M`, `c` are toggles).

**Everyday**

| Key | Effect |
|-----|--------|
| `?` | identify — firmware version, chip, PSRAM, MAC, mesh, mute, trim, whether BT and ESP-NOW are up |
| `N` | where this node sits in the mesh: its mode, its source (`phone`, `bench`, or the MAC of the server it is locked to), and `rx`/`lost`/`und` in this stream. One line, so `@* N` lists the mesh |
| `@` | `@<name\|mac\|*> <command>`: run the command on that node, or on every node of this mesh, and print the answers. See above |
| `v` | `v<dB>` this node's own volume trim, −40…+12; bare `v` reports. Applied at this node's output, after the mesh has its copy, so the phone's slider still moves every room and the trim sets where this room sits among them. Kept in NVS |
| `m` | mute this node's speaker until reboot — the mesh and every timing are untouched. A toggle |
| `M` | client: mix stereo to mono on both channels, for a node with one speaker (a MAX98357A plays one channel). Kept in NVS. A toggle |
| `g` | print the mesh identity; `g<name>` sets it. Kept in NVS, takes effect at once |
| `p` | listen for 60 s and join the mesh that offers itself — the node half of pairing |
| `o` | offer this mesh for 60 s — the server half |
| `c` | toggle client-only mode and reboot: the node never starts Bluetooth, which is what lets a WROOM be a mesh client. Kept in NVS |
| `U` | bare: reboot into update mode. `U<name>` asks that node over the mesh — what `ota.ps1` sends |
| `W` | `W<ssid>`, then the password on the next line: the home WiFi update mode joins. Bare `W` prints the SSID and whether a password is stored, never the password. This node's port only |

**Measuring** — what `bench-mesh.ps1` and the tools in `tools/btlisten/` drive

| Key | Effect |
|-----|--------|
| `b` | reboot into bench mode (Bluetooth stays off) |
| `n` | reboot into normal mode |
| `s` | start generating the synthetic test stream |
| `x` | stop generating it |
| `r` | print a telemetry line now |
| `d` | toggle clock-drift correction (on by default) |
| `l` | client: print and reset the histogram of lost-run lengths (1..7, 8+) and `rec`, the blocks rebuilt from a later packet or a repeat copy |
| `L` | client: `L1` prints which packets were lost, a bit each, once a second as `[LT]` lines; `L0` stops. What `tools/btlisten/losstrace.py` scores redundancy schemes on |
| `z` | client: `z1` fills a block that was lost and not rebuilt from its neighbours, `z0` plays zeroes. `MESH_CONCEAL` is the default |
| `t` | `t<n>` sends each mesh frame n times (1–3) until reboot |
| `D` | `D<n>` each packet carries the block n packets back as well (0–15) until reboot; on the wire, so clients follow |
| `X` | `X<n>` each packet carries the XOR of the blocks 1 and n back instead (2–15) until reboot |
| `P` | `P<us>` spaces audio packets at least that far apart, until reboot |
| `R` | `R<Mbps>` sets the ESP-NOW PHY rate this node sends at, until reboot: 1, 2, 6…54 |

**Bluetooth server**

| Key | Effect |
|-----|--------|
| `a` | print and reset the A2DP window — packets/s, a histogram of the gaps between packets from the Bluetooth stack, and the server's own ring. A gap longer than the ring holds is a hole in every room |
| `f` | toggle forwarding to the mesh; local playback carries on |
| `w` | stop WiFi until the next reboot — the WROOM case, on a WROVER |
| `j` | play the connect jingle now, as a connection does, into the server's own output |
| `k` | `k<aa:bb:cc:dd:ee:ff>` dials a bonded A2DP source, as a headset reconnects to a phone; the PC's address is in `boards.local.md` |
| `V` | `V<0..127>` the A2DP volume, as a phone's slider would set it: it moves every room |
| `e` | `e<n>` coexistence preference, 0 WiFi, 1 Bluetooth, 2 balance (default). Made no measurable difference |

Bench mode exists because the server role needs a phone to connect, which
cannot be automated. It never starts Bluetooth, so it also runs on a board with
no PSRAM — which is how a WROOM is tested on the mesh at all (D9).

## Libraries

- [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) — Bluetooth A2DP Sink

## License

MIT License
