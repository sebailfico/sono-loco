# Design Decisions

Why SonoLoco is built the way it is. Each entry records the reasoning at the time
and — more usefully — **what would change it**.

This file exists because the same questions keep coming back ("why not just use
Bluetooth for everything?", "why can't the WROOM join?") and re-deriving the
answer from scratch each time is both slow and unreliable. If you revisit one of
these and the conclusion changes, edit the entry and say so; don't delete it.

---

## D1 — One firmware image, roles negotiated at runtime

**Decided:** at project start. **Status:** holding.

Every node flashes the same firmware and picks SERVER or CLIENT at runtime:
whichever node the phone connects to becomes the server, the rest follow. There
is no configuration step, no "master" node, no router and no central server.

The alternative — a compile-time server build and a client build — is simpler to
write and much worse to live with: you have to remember which board is which,
re-flash to rearrange rooms, and the server becomes a single point of failure.

**What would change this:** nothing short of the runtime negotiation proving
unworkable on hardware. It is the point of the project rather than an
implementation detail.

---

## D2 — ESP-NOW for the mesh, not a second Bluetooth link

**Decided:** 2026-08-19, after explicitly re-examining it. **Status:** holding.

The obvious-looking simplification is to drop ESP-NOW and have the server relay
audio to the other nodes over Bluetooth as an A2DP *source*, giving the project
one radio protocol instead of two. It does not work:

1. **A2DP is point-to-point; there is no broadcast.** The vendored library keeps
   a single `peer_bd_addr` and calls `esp_a2d_connect()` on it
   (`ESP32-A2DP/src/BluetoothA2DPSource.cpp`). Three rooms means three
   independent AVDTP streams, three SBC encoders and three sets of ACL traffic
   sharing one radio. One ESP-NOW broadcast frame reaches every node at once —
   that is what makes the mesh scale at all.
2. **Sink and source at the same time is a scatternet.** As the phone's A2DP
   sink the node is a *slave* in the phone's piconet; to feed clients it must be
   *master* of its own. The ESP32's BR/EDR controller is not designed for that
   combination, and tellingly the library ships no combined sink+source class.
   This would be an open-ended research detour, not a swap.
3. **It removes the only lever on the sync problem.** ESP-NOW hands us raw
   packets, so we own the jitter buffer and can timestamp and drift-correct —
   which is exactly the fix the open timing issues need. A2DP hands buffering to
   the peer's stack: opaque, variable per link, no shared clock.

Smaller costs: tandem SBC coding (decode the phone's stream, re-encode per
client) degrades already-lossy audio and burns CPU per stream; and it would drop
the S3/C6 as node types entirely, since they have no BT Classic.

The one real upside is that a BT-only mesh needs no WiFi, which frees the ~80 KB
that currently locks the WROOM out (see D3). That is not worth points 1-3.

**What would change this:** LE Audio / Auracast becoming available on an
Espressif part we can buy. Broadcast Isochronous Streams are genuinely the right
protocol for this — one transmitter, many receivers, sync built into the spec.
No ESP32 variant in use here supports it. Also worth re-checking point 2 against
current ESP-IDF before ever acting on it; it is the claim here that rests least
on something directly verified in this repo.

---

## D3 — The WROOM is deliberately excluded from the mesh

**Decided:** during the first audit. **Status:** holding, and it is not a bug.

Running BT Classic and WiFi simultaneously on an ESP32 needs PSRAM. Without it,
WiFi claims ~80 KB of the ~215 KB DRAM heap and the BT stack can no longer
allocate its L2CAP/AVDTP buffers, so it crashes. `setupESPNow()` therefore bails
out early on a board that has BT compiled in and reports no PSRAM.

A WROOM node is a plain Bluetooth speaker with no mesh. The WROVER is the only
currently-stocked module that is fully interchangeable.

**What would change this:** shrinking the mesh's RAM footprint far enough that
BT and WiFi coexist without PSRAM — unlikely, the 80 KB is the WiFi driver's own
buffers, and the static RX buffers cannot be moved to PSRAM because they must be
DMA-capable internal DRAM. Realistically: buy WROVERs.

---

## D4 — One binary for WROOM and WROVER, three environment names

**Decided:** during the second audit. **Status:** holding.

There are two builds because there are two instruction sets, and three
environment names because there are three boards with three COM ports.
`esp32dev` and `esp32wrover` compile identical code.

This works because the Arduino core ships `CONFIG_SPIRAM=y` with
`CONFIG_SPIRAM_BOOT_INIT` unset, so `psramInit()` probes at boot and only logs a
warning when there is no PSRAM. One image boots on both modules and D3's runtime
check decides what the node can do.

**What would change this:** nothing should. If a board needs different
*behaviour*, detect it at runtime — adding a third build config re-introduces
"which binary is on which board?", which is the problem D1 exists to avoid. The
S3 stays separate only because it is a different architecture with no BT Classic
and the A2DP library will not compile for it.

---

## D5 — Mesh audio is 22.05 kHz mono

**Decided:** at implementation time. **Status:** holding, with a known cost.

BT A2DP delivers 44.1 kHz stereo 16-bit = 176 KB/s. Broadcasting that over
ESP-NOW while BT Classic shares the same radio is not realistic, so the server
halves the sample rate and folds stereo to mono: 4x reduction, 44 KB/s,
~220 packets/s of 200 bytes.

Decimation goes through a 4-tap `[1 3 3 1]/8` FIR. Naive 2:1 decimation folds all
11-22 kHz content back into the audible band and turns cymbals to fizz.

**Known cost:** the server plays the full 44.1 kHz stereo stream locally, so the
server room and the client rooms do not sound identical. Unresolved — see
`TODO.md`.

**What would change this:** IMA ADPCM (4:1, cheap) would take the same audio to
~11 KB/s, which would buy back enough headroom to reconsider the sample rate or
the stereo fold. That is the first thing to try if bandwidth turns out to be the
binding constraint on the bench.

---

## D6 — Broadcast plus first-sender lock, instead of pairing

**Decided:** during the first audit. **Status:** holding.

The server broadcasts to `FF:FF:FF:FF:FF:FF` on a fixed channel rather than
maintaining a peer list. Nodes need no knowledge of each other, which is what
makes "flash it and it works" true.

The cost is that two servers broadcasting at once would interleave into one
jitter buffer and produce noise, so a client locks onto the MAC of the first node
it hears and ignores everything else until it returns to DISCOVERY. The lock must
be cleared on *every* entry to DISCOVERY — a stale lock made a node ignore every
future server permanently, which was a real bug.

**What would change this:** wanting more than one independent audio zone in one
home. That needs a group identifier in the packet header, not a peer list.

---

## D7 — Pure logic lives in `lib/`, and is tested on the host

**Decided:** 2026-08-19. **Status:** new.

The ring buffer and the packet sequence accounting are in `lib/jitter/` rather
than in `main.cpp`, with host tests in `test/test_jitter/`.

The argument is not general tidiness — `main.cpp` is otherwise deliberately one
file. It is that this specific code is (a) pure logic with no hardware
dependency, and (b) where every hard-to-see bug has come from: a permanent 16-bit
framing shift from a partial buffer write, and a reordered packet accounted as
65535 lost packets. Neither is obvious on a board; both are trivial to pin down
in a test that runs in a second.

**What would change this:** nothing, but resist widening it. Code that touches
I2S, the radio or the A2DP callbacks stays in `main.cpp` where it can be read in
one pass.

---

## D8 — `ROOM_NAME` is set per environment, not in `config.h`

**Decided:** 2026-08-19. **Status:** new.

Per-node naming used to mean editing `config.h` before each flash, which made the
source tree differ per board and left no way to tell which binary was on which
node. It now comes from `platformio.ini`, where the per-board environments
already live, so the tree is identical for every node and you flash an
environment rather than an edited file.

It is also the Bluetooth advertised name, so distinct values mean a phone sees
`SonoLoco-WROVER` rather than three identical `SonoLoco` entries — which matters,
because "connect to any node" is the product.

**What would change this:** storing the name in NVS and setting it at runtime,
if nodes ever get a configuration interface.
