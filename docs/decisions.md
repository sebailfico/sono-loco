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

## D3 — A WROOM is a Bluetooth speaker or a mesh client, never both

**Decided:** during the first audit, as "the WROOM is excluded from the mesh";
narrowed 2026-08-19 to this. **Status:** holding. A WROOM in client-only mode
plays the mesh through a stereo across the room (2026-10-01).

Running BT Classic and WiFi simultaneously on an ESP32 needs PSRAM. Without it,
WiFi claims ~80 KB of the ~215 KB DRAM heap and the BT stack can no longer
allocate its L2CAP/AVDTP buffers, so it crashes. `setupESPNow()` therefore bails
out early on a board that has BT compiled in and reports no PSRAM.

A WROOM node left in its default mode is a plain Bluetooth speaker with no mesh.
The WROVER is the only currently-stocked module that is fully interchangeable —
the only one that can be a server and still switch to being a client.

**Amended 2026-08-19, and this matters:** the exclusion is narrower than it was
written. The conflict only exists while the BT stack is *running*. A WROOM that
never starts Bluetooth runs WiFi and ESP-NOW perfectly well with no PSRAM —
measured, not assumed: a WROOM was the ESP-NOW source for a 600 s run at
220.5 packets/s with `qfull`, `senderr` and `radiofail` all zero, and not one
packet lost.

So a WROOM cannot be a **server**, because that needs BT and WiFi together. It
can be a **client**, because that needs only WiFi.

**Implemented 2026-08-20:** client-only mode, set with the serial command `c` and
stored in NVS. A node in it never starts Bluetooth, so `setupESPNow()` lets it
have the radio, and a WROOM becomes a real mesh node. Measured on the shipping
path rather than in bench mode — normal boot, ordinary status output — a WROOM
took 12,870 packets in 60 s with zero overflow, zero underrun, a jitter buffer
holding 2,880 bytes and 93 KB of heap still free. `tools/test-client-only.ps1`
reproduces it.

It is stored in NVS rather than RTC memory, unlike bench mode, because it is a
*setting*: a node wired into a room has to come back as what it was after the
power blinks. Bench mode staying volatile is equally deliberate — a board left in
a test mode by a power cut is a trap.

**Amended 2026-09-14, first WROVER on the bench:** BT and WiFi together works
as designed — `SERVER capable`, ESP-NOW up, BT discoverable — with 15.5 KB of
internal DRAM free in DISCOVERY. That is the first real number for the "BT +
WiFi together" heap question and it is smaller than the ~80 KB reasoning above
implies for a board with PSRAM: PSRAM absorbs large allocations, but the WiFi
static RX buffers, the BT controller and every allocation below the
always-internal threshold still come out of DRAM. Also learned the hard way:
coexistence forbids `WIFI_PS_NONE` on a BT node (README gotcha), so a BT node
runs WiFi with modem sleep on, while a BT-free node still turns it off — which,
measured 2026-10-01, costs a client no packets.

**Amended 2026-09-14, evening:** BT and WiFi *together* now has two numbers.
Memory: a phone connecting needs more than the 15.5 KB of internal DRAM the
node had; after releasing the BLE half of the controller, fixing a WiFi TX
trim that had never applied, and moving what could move to PSRAM, it has 43 KB
and holds 18–32 KB while streaming. Radio: with A2DP streaming, about a fifth
of the ESP-NOW frames handed to the radio never reach the air intact — 215/s
sent, ~170/s seen by a monitor 30 cm away, ~153/s at a client. The
coexistence arbiter is real and it is the binding constraint on a one-chip
server. What came of that is D5 (fewer bytes) and D13 (shorter frames,
redundancy); a coexistence preference made no difference; a two-chip server
is still the fallback that removes the question.

**What would change this:** shrinking the mesh's RAM footprint far enough that
BT and WiFi coexist without PSRAM — unlikely, the 80 KB is the WiFi driver's own
buffers, and the static RX buffers cannot be moved to PSRAM because they must be
DMA-capable internal DRAM. What remains true is the shape of the conclusion: a
WROOM is a server or a client, never both, and which one it is is now a setting
rather than an accident of what the board has soldered to it.

---

## D4 — One build per instruction set, one environment name per board

**Decided:** during the second audit. **Status:** holding; the count has changed
twice, the rule has not.

There is one build per instruction set — three now: classic Xtensa, S3, and the
RISC-V C3 added 2026-08-20 — and one environment name per board that might be
plugged in, because each needs its own COM port. `esp32dev` and `esp32wrover`
compile identical code.

This works because the Arduino core ships `CONFIG_SPIRAM=y` with
`CONFIG_SPIRAM_BOOT_INIT` unset, so `psramInit()` probes at boot and only logs a
warning when there is no PSRAM. One image boots on both modules and D3's runtime
check decides what the node can do.

**What would change this:** nothing should — but read the rule carefully, because
the number is not the rule. A build config is for silicon that cannot execute the
same instructions. The S3 and C3 have their own because they are different
architectures with no BT Classic, and the A2DP library will not compile for
either. A board that merely needs to *behave* differently must not get one: that
re-introduces "which binary is on which board?", which is the problem D1 exists
to avoid. When a WROOM needed to be a client rather than a server, the answer was
a runtime setting stored in NVS (D3), not a fourth build.

---

## D5 — Mesh audio is 44.1 kHz stereo IMA ADPCM (was 22.05 kHz mono PCM)

**Decided:** at implementation time (22.05 kHz mono); **changed 2026-09-29**
to 44.1 kHz stereo ADPCM. **Status:** holding; through a stereo on 2026-10-01
it "works incredibly well".

BT A2DP delivers 44.1 kHz stereo 16-bit = 176 KB/s, too much to broadcast over
ESP-NOW with BT Classic on the same radio. The first mesh took a quarter of it
the simple way: half the rate, stereo folded to mono, 44 KB/s, through a
`[1 3 3 1]/8` FIR so the decimation did not fold 11–22 kHz into the audible
band. The cost was that the server played the full stream and the clients did
not, and the note here said IMA ADPCM (4:1, cheap) was the thing to try.

**Changed 2026-09-29 — 44.1 kHz stereo, through IMA ADPCM.** That is what the
note said to try, spent the other way: not the same audio in a quarter of the
bytes, but the server's own audio in the same bytes. The decision
was made by ear before any firmware was written: `tools/codec/abtest.py` ran two
20 s excerpts (drums and cymbals; a quiet acoustic track) through the old path
and through ADPCM, and the listener found ADPCM "way better". The numbers
agree: the old path was 8–10 dB down above 8 kHz and 31–36 dB down above
12 kHz — the `[1 3 3 1]/8` FIR rolls off long before the 11 kHz Nyquist —
and had no stereo at all; ADPCM keeps both, at 28–31 dB SNR. The noise it adds
follows the music's level and was judged "pretty noisy" when isolated and
turned up, and not a reason to stay.

So the server forwards what A2DP gives it: no fold, no decimation, no FIR. A
packet carries its own 114-frame block and an older one for redundancy (D13),
387 packets/s of 248 bytes. Every block carries the decoder's state, so any
block decodes alone — which is what lets a client rebuild a lost block from a
later packet. Every room plays what the server plays. A node with one speaker
mixes to mono (`M`), because a MAX98357A plays only one channel.

**What would change it now:** the noise, if somebody hears it. The first real
listening, 2026-10-01 — a WROVER server, a WROOM client into a stereo — asked
for nothing. If it ever is audible, the next step is not back to PCM but a better codec: SBC is
what the phone already sends, but this Arduino core hands over only decoded
PCM, so the server would re-encode. Or an ADPCM variant with more bits per
sample, if the airtime allows it.

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

**Amended 2026-09-02 — the group identifier now exists.** Every packet carries a
16-bit mesh id and a client drops anything that is not its own, so two
households in radio range no longer join each other's music. Nothing about this
decision is undone by it: the destination is still the broadcast address, there
is still no peer list, and the lock is still first-sender — it is now
first-sender *within my mesh*. The mesh check runs **before** the lock, so a
neighbour's server can never take it. See D12.

**What would change this:** wanting a client to follow one *specific* node
rather than the first one in its mesh — two servers in one household, say, one
per floor. That is addressing, and a peer list finally becomes the honest
answer.

---

## D7 — Pure logic lives in `lib/`, and is tested on the host

**Decided:** 2026-08-19. **Status:** holding; five libraries now (`jitter`,
`adpcm`, `mesh`, `drift`, `sync`), each with its suite.

The ring buffer and the packet sequence accounting were the first to move to
`lib/jitter/` rather than stay in `main.cpp`, with host tests in
`test/test_jitter/`.

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

**Decided:** 2026-08-19. **Status:** holding. A board still advertising
`Room1`, the old default in `config.h`, is running firmware from before it.

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

---

## D9 — Bench mode is a runtime mode, not a build

**Decided:** 2026-08-19. **Status:** holding.

Automated multi-board testing needs a node that produces a stream unattended.
The normal SERVER role cannot: it requires a phone to connect over A2DP. So a
node can be told over serial to reboot into **bench mode**, where it never starts
Bluetooth and can generate a synthetic tone straight into the ESP-NOW transmit
path — the same encoder, packets and schedule stamps as a server's.

It is a runtime mode rather than a `-DBENCH` build for the reason D4 gives: the
test should exercise the same binary that ships. The flag lives in
`RTC_NOINIT_ATTR` memory so it survives the restart that is needed to take effect
before `setup()` decides whether to start Bluetooth. `RTC_DATA_ATTR` does not
work here — `.rtc.data` is re-initialised from the image on every boot that runs
the bootloader, so the flag is already zero again by the time it is read.

Because bench mode never starts Bluetooth, it also works on a WROOM (see D3),
which is what made a two-board test possible at all with the hardware to hand.

**What would change this:** nothing foreseen. The *Bluetooth* path is outside
it by construction; that half is driven from the PC instead
(`tools/btlisten/`, the PC as the A2DP source).

---

## D10 — The version comes from git, not from a constant

**Decided:** 2026-08-20. **Status:** holding.

`scripts/version.py` runs before every build and defines `FW_VERSION` from
`git describe --tags --always --dirty=*`. The node prints it in its boot banner
and reports it in `[BENCH] id fw=…`, so the bench harness records which build
produced each measurement, and warns when a board is running something other
than the tree in front of you.

The version is derived rather than declared because the question it has to answer
is "is this board running the code I am reading?", and a hand-maintained constant
in `config.h` cannot answer it: it is correct only for as long as everyone
remembers to bump it, and the failure is silent. The one thing that is always
true and always available is what git already knows.

Tags supply the human-readable part. `v0.1.0` is the first hardware-proven mesh;
untagged commits describe themselves as `v0.1.0-3-gabc1234`, which is still an
exact reference to a tree. The trailing `*` on a dirty build is the important
part of the string — it means the sha does *not* describe what is on the board,
so any number measured from that build is unreproducible, and the harness says so
rather than letting it into `CHANGELOG.md` unnoticed.

CHANGELOG entries are headed with the version they describe for the same reason:
a baseline like "-30.5 ppm" is only useful if the firmware that produced it can
be rebuilt.

**What would change this:** nothing about the mechanism. The tagging *policy* is
worth revisiting once there is a second person flashing boards, or an update
path that has to decide whether an image is newer than the running one —
`git describe` output does not order without parsing, and that would be the
moment to put a real semver in the tag and compare against it. D15's update
mode did not bring that moment: it never compares versions — the PC decides
what to send, and reads `fw=` back afterwards to check it.

---

## D11 — Drift is corrected by duplicating and dropping single samples

**Decided:** 2026-08-20. **Status:** measured on hardware. WROOM source,
ESP32-C3 client, 600 s each way: -57.7 ppm uncorrected with an underrun, flat and
underrun-free corrected, and the two independent measures of the drift agreeing
within 1.4 ppm. See `CHANGELOG.md` v0.2.0.

Two crystals, nothing synchronising them: the client consumes at a slightly
different rate than the source produces, and the jitter buffer slowly empties or
fills. Measured over 600 s on the first pair of boards, -30.5 ppm, which drains
the buffer in about 18 minutes of continuous play. Every client has to correct
for it or eventually break.

The correction is **one frame, duplicated or skipped, at a batch boundary** —
a mono sample when this was decided, a stereo frame since D5. 30 ppm at
44.1 kHz is 1.3 frames a second, each held for one extra sample period. That
is far below audibility and it costs nothing: no filter state, no
fractional-delay interpolation, no per-sample arithmetic in the audio path at
all.

The alternative is a real asynchronous sample-rate converter, which resamples the
whole stream by the measured ratio. It is the correct answer for a system that
has to survive large or fast-changing offsets, and the wrong one here: it would
add a resampler to a 240 MHz core that also runs a radio, to fix an error of
0.003%. This entry said the trade would change if the mesh ever carried stereo
at a higher rate; it now does (D5), and the trade held.

**It is a controller, not a constant.** -30.5 ppm is one pair of crystals at one
temperature; a third board has a different offset, and the same board has a
different one when it is warm. That was the argument in advance; the hardware
then made it directly — the same WROOM source measures -30.5 ppm against the S3
and -57.7 ppm against the C3, a factor of nearly two between two clients of one
source. So the firmware steers on the *buffer occupancy*
it can actually see, rather than on a number measured once. Proportional control
on the smoothed fill error with a deadband — the deadband is what keeps it from
chasing packet-arrival jitter, and it costs a slightly shallower buffer in
exchange. The full reasoning, including why there is no integral term, is in
`lib/drift/drift.h`. Since D14 a client on the server's schedule steers the
same controller on its timing error instead of its depth; the server, and a
client of a source that sends no schedule, still steer on depth.

Placing the edit at a batch boundary rather than hunting for a zero crossing is
also deliberate: the partial-write accounting in `driveRingI2S` is the code
that once shifted the 16-bit framing permanently, and an edit in the middle of a
buffer that I2S may only half accept is exactly how that bug would come back.
One sample either side of a write whose length is already handled correctly
cannot do that.

**Two things the hardware corrected about the design, both worth keeping:**

*The target is measured, not computed.* The ring is not where all the buffered
audio lives — the DMA ring holds part of it — so the level to steer to is the
prefill less whatever the DMA is holding. Computing that from the DMA ring's
*capacity* assumes it sits permanently full, and it does not: measured, it holds
about 1,880 of 2,048 bytes. The controller now takes the level it observes once
its settle window closes. The first version steered 350 bytes too shallow, and
before that, steering to the full prefill was 1,600 bytes out in the other
direction.

*The gain is set by the depth the buffer has to keep.* Proportional control parks
the level at `target - (deadband + rate/kp)`, so the gain decides how deep the
buffer runs in steady state, and that depth has to survive a radio hiccup. The
first gain parked a -58 ppm client at ~1,290 bytes; the baseline run then
underran on a two-packet loss with 1,304 bytes showing a second earlier. This is
the sort of thing that is invisible in the code and obvious in a 600 s run.

**What would change this:** material where a held sample is audible, a client
whose offset exceeds what the controller is allowed to correct, or a sample
rate that stops being fixed at 44.1 kHz (`TODO.md` — if A2DP negotiates
48 kHz, the whole rate assumption changes and an SRC may be needed anyway).

---

## D12 — A mesh is a 16-bit id in every packet, derived from a name

**Decided:** 2026-09-02. **Status:** holding. Covered by host tests and in use
on every node; not yet measured with two meshes in the air at once, and the
pairing button has not been pressed.

Two SonoLoco installations within radio range hear each other perfectly: the
channel is fixed, the destination is the broadcast address, and a client locked
onto whichever server it heard first (D6). Which house's music a client played
was decided by who powered up first.

Every packet now begins with a 16-bit mesh id, and a client drops a packet whose
id is not its own before it looks at anything else.

**Why a field in the header, not a peer list or a channel.** Two bytes take the
frame from 204 to 206 of the 250 ESP-NOW allows, and cost nothing measurable at
220 packets/s. A peer list would undo what makes this project work at all — no
node knows about any other, so a new board just works (D6). A channel per mesh
would separate the RF as well as the logic, which is genuinely attractive, but a
client can then no longer sit on a fixed channel to discover and has to scan; it
stays an open question in `TODO.md` rather than a thing done in passing.

**Why a name, hashed, rather than a number.** People own the mesh, not the id:
"our mesh is called Casa Rossi" is something a person can say, remember and type
into a second board. The id is FNV-1a over the normalised name, folded to 16
bits — see `lib/mesh/`. The cost is a 1-in-65536 chance that two neighbouring
names collide, which pairing avoids entirely by copying the id off the air
instead of deriving it.

The normalisation is not cosmetic. Two nodes must agree that " Casa  Rossi " and
"casa rossi" are the same mesh, because a disagreement is *silence with nothing
in the log* — a client with the wrong id is indistinguishable from a client out
of range. That is why the derivation lives in a library with host tests instead
of inline in `main.cpp`, and why the tests pin known names to known ids: a
future change to the hash would split every deployed mesh silently, upgraded
boards computing one id from the stored name and not-yet-upgraded ones another.

**Why NVS, and why that is a stronger argument than it was for client-only
mode.** The mesh id is per *household*, so unlike `ROOM_NAME` (D8) it cannot
live in `platformio.ini`: it has to be settable on a board somebody already owns
and has already screwed to a wall. It also has to survive a power cut, because a
node that came back on the factory default would silently rejoin whichever
neighbour is still on that default.

**The default is shared, deliberately.** A fresh set of boards all carry
`MESH_NAME` from `config.h`, so one household still flashes and it works. Two
households that both accept the default are in one mesh, exactly as they are
today — the second one presses a button. Per-device random ids would isolate
everybody out of the box and make *every* install a pairing exercise, to fix a
problem only the second household in earshot actually has.

**Pairing takes a press at each end, and adopts an id rather than a name.** Hold
the button on a node of the mesh being joined and it *offers* itself for 60 s;
hold the button on the node being moved and it *listens*, adopting the first
offer it hears. Copying an id cannot land in the wrong mesh through a name
collision, and it needs no console — which is the point, since the board it runs
on is on a wall. The name is lost in the process (only the id travels on the
wire), so a paired node logs `(paired)` where a named one logs its name.

The first version adopted the first foreign *stream* heard, which was one press
and a real hole: a neighbour who merely started playing music during the window
would capture the node, and nothing about that requires them to intend it. With
an offer required, somebody has to be standing at the other mesh pressing its
button inside the same minute.

Which half a press performs follows the node's role rather than asking: a node
that can be a server offers the mesh, a speaker joins one. That matches where
the two boxes physically are, and the serial console keeps both halves (`o` and
`p`) for the cases the button cannot express — moving a server into another
mesh, most obviously. With one caveat there: a node actively serving a phone
does not listen at all, because the receive path bails out in SERVER mode
(D6). Move such a node by naming its mesh with `g`, or disconnect the phone
first.

**A beacon is an audio packet with no payload**, carrying `MESH_BEACON_SEQ` in
the sequence field and the mesh id in the header, which is the entire message.
Reusing the audio layout meant the wire format did not have to break twice, and
it goes out through the existing queue and send gate rather than a second
transmit path that could rot. Both halves of the test matter: length alone would
promote any truncated frame to a beacon, and the magic alone would make one
audio packet in 65536 — a false beacon every three minutes at 387 packets/s — an
invitation to join a stranger. A beacon must also be dropped *before* the
sequence tracker sees it, or its sequence number reads as a stream restart.

**The tones are not decoration.** Two beeps when a window opens, the rising
three-note tone on adoption, a falling one when a window closes empty. Pairing
is used by somebody holding a button on a box with no screen and no console, and
without them the button is indistinguishable from a button that does nothing.
They play only in DISCOVERY: in CLIENT and SERVER mode `loop()` is feeding the
I2S driver from the ring.

It is a long press **while running**, never a press held through a reset. The
BOOT button these devkits use is a strapping pin — held across a reset it puts
the chip into the ROM download mode, where none of this firmware runs at all.

**This is isolation, not privacy.** ESP-NOW encrypts only unicast frames, with a
per-peer LMK; a broadcast frame cannot be encrypted at all, which is the
topology this design rests on. So the id keeps a neighbour's *player* out, not a
determined neighbour's *receiver* — anyone running modified firmware can still
listen. Fixing that means encrypting the payload under a mesh-derived key, on a
client that is already doing I2S, drift correction and a radio.

**What would change this:** more than 65,536 plausible households in one
building (it will not be), a need for actual confidentiality rather than
separation, or a decision to give each mesh its own channel — at which point the
id stays but stops being the only thing keeping the two apart.

---

## D13 — ESP-NOW frames go out at 12 Mbps, twice, not once at the 1 Mbps default

**Decided:** 2026-09-14 (6 Mbps), amended 2026-09-29 (12 Mbps, two copies),
2026-09-30 (the second copy 11 packets later).
**Status:** holding. Range: across one room, at a stereo, 240,425 packets
and none lost (2026-10-01); further than that untested.

ESP-NOW sends broadcast frames at 1 Mbps DSSS unless `esp_wifi_config_espnow_rate`
says otherwise. That is the most robust rate 802.11 has — the best receiver
sensitivity, and DSSS lets a strong receiver decode straight through a weaker
interferer — and it is also the slowest: a 206-byte SonoLoco frame is about
2.2 ms of air, and 220.5 of them a second is roughly half the channel. Half the
channel, on channel 1, with neighbours.

Measured on five boards, WROOM sourcing to two WROVERs, an S3 and a C3, 600 s
each, same afternoon, same positions. At 1 Mbps the four clients lost 0 / 280 /
2,955 / 14,079 packets — the strong receivers decoded through the interference
and the weak ones did not. At 6 Mbps OFDM they lost 86 / 708 / 1,493 / 1,495:
total loss 4.6× lower, the worst board 9× better, the two best boards
slightly worse, and the C3 and S3 now losing the *same* frames, which is what a
short OFDM frame under a burst of interference does — it dies for everyone or
for no one. When the interference stopped, all four were clean at 6 Mbps.

So 6 Mbps trades a few dB of sensitivity for a sixth of the airtime. On a
bench that is the right trade. The open question is whether it still is two
rooms away, where the few dB may be the difference between hearing the source
and not — `TODO.md` has the range test.

**What was considered:** 2 Mbps (halves the airtime, keeps DSSS capture — worth
measuring if OFDM loses at range); MCS0 (6.5 Mbps HT, same sensitivity class as
6 Mbps, no advantage without 40 MHz); anything faster (11n MCS3+ buys airtime
nobody needs and sensitivity everybody does). Changing the channel is not an
alternative to this, it is the other half of it: see the channel item in
`TODO.md`.

**Amended 2026-09-29 — 12 Mbps, and every frame twice.** The first real
Bluetooth server (the PC streaming into a WROVER) lost 12–24% of its mesh
frames, every loss a single frame, with nothing else on the channel and the
client's own Bluetooth off: the server loses them to its own BT link, and a
broadcast has no retry. Shorter frames lose fewer, by more than their length
alone would suggest — one copy each, same session: 6 Mbps 12%, 12 Mbps 2.0%,
24 Mbps 4.0%, 54 Mbps 1.6% — and a second copy sent straight after the first
recovers almost every single loss. 12 Mbps with two copies lost nothing in
15 s and 0.094% over 600 s, for about the airtime one copy at 6 Mbps used
(`CHANGELOG.md`). 12 rather than 54 because it costs about 4 dB of sensitivity
against 6, where 54 costs 17, and bought nearly as much. The copies are marked
in the header (`ESPNOW_LEN_REPEAT`) so a client's counters still count blocks.

**Then, the same day, the redundancy moved inside the packet.** With ADPCM
(D5) a 114-frame block is 120 bytes, so a packet has room for its own block and
the previous one: every block goes out twice, 2.6 ms apart, instead of in two
copies 0.3 ms apart — the bursts that took both copies of a frame were the
losses left over. The copies are back to one (`ESPNOW_TX_COPIES`, and `t2` still
sends two for comparison). The rate stays 12 Mbps.

**What was considered:** the coexistence preference (WiFi first: no change,
332 lost against 338); 2 Mbps (behind a BT server every frame waited 20–50 ms
for the radio and the client dropped out — so it is no longer the fallback
below); unicast to known clients, which gets the MAC's own retries for free
but needs a peer list (D6) and multiplies the airtime by the number of rooms;
and ADPCM with the previous block in every packet — done the same day, above.

**Amended 2026-09-30 — the second copy goes 11 packets later, not 1.**
Measured behind a streaming server for the first time, the previous-block
scheme left 1–3% holes. Both clients — a WROVER and an S3 — lost the *same*
packets, run for run, so the frames never left the server. The server loses
them in runs of 4–5 packets (10–13 ms) when its Bluetooth link has the
radio, besides isolated singles; a block one packet back died in the same
run. So a packet now carries its block and the one `MESH_REDUNDANCY_DISTANCE`
packets back, and a client writes the late block over the silence it played
in its place, if that has not played yet (`JitterBuffer::patch`). The
distance is on the wire (`len` bits 8–14), so a server's `D<n>` moves the
whole mesh and the three settings could be alternated in one session: holes
1.17% at 1, 0.57% at 6, 0.33% at 11 (`CHANGELOG.md`); over the next 600 s,
0.7%. Any distance above 1
has a price the numbers show: a lone lost packet is no longer always saved,
because its copy can land on another loss — in the minutes when the server
dropped only singles, distance 1 did best. The floor for any one-copy scheme
is the server's own loss rate squared, and that rate was 3–9%.

**What was considered, 2026-09-30:** pacing the server's sends
(`ESPNOW_TX_PACE_US`, `P<us>`): an A2DP packet from Windows becomes nine mesh
packets queued at once, and spacing them 2 ms apart doubled the single
losses and thinned the runs without changing the holes, so it stays off. Two
back-to-back copies (`t2`) plus the distance would lower the floor again, at
twice the airtime. Three blocks a packet would need shorter blocks, and more
packets. And the fix at the source: a server that loses fewer frames — see
the next paragraph.

**Measured 2026-10-01, on one recording.** Every scheme compared on the same
losses (`L1`, `losstrace.py`), 600 s behind the streaming server at 6.4% loss:
copy at 11 left 0.50% holes, the best copy distance (15) 0.43%, the XOR of
blocks 1 and 11 back 0.29%, and of 1 and 15 back 0.24%. The XOR's near half
rebuilds a lone loss from the next packet and its far half a run from 15
later, in the same bytes. It is already in the firmware (`X<n>`) and becomes
the default once a live run confirms the firmware rebuilds what the model
says it does (`TODO.md`).

**What would change this:** that confirmation, first. Then a range
measurement showing 6 Mbps reaching a room that 12 does not — then 9 Mbps, or
the copies alone at 6, before anything slower. A server with no Bluetooth of its own (the two-chip server in
`TODO.md`) would make the copies a hedge against interference only, and worth
re-measuring against their airtime — and would remove the 4–5-packet runs
the distance exists for, so distance 1 might win again. Or ADPCM shrinking the frames so far that
the rate stops mattering.

---

## D14 — Every node plays on the server's schedule, the server included

**Decided:** 2026-09-30. **Status:** measured on hardware. By microphone,
behind the PC streaming into WROVER1, the S3 +0.8 ms and WROVER2 +1.5 ms from
the server, against +51 and +44 before. By telemetry, both within ±0.55 ms of
the server's schedule for 600 s, with no underruns and no jumps. See
`CHANGELOG.md`.

Measured with `tools/btlisten/sync.py` before any of this, clicks through
the PC into WROVER1: the S3 played **51 ms** after the server and WROVER2
**44 ms** after it. Anyone standing between two rooms heard an echo. There
were two causes, and neither is visible in any counter:

- *A fixed difference in latency.* The server played through the A2DP
  library's I2S output, a 46 ms DMA ring. A client played through its
  jitter buffer, and then through its own 46 ms DMA ring behind that.
- *A random one on top.* A client started playing when its ring reached the
  prefill, and that happens part way through a burst: Windows sends a
  1024-frame packet every 23 ms and it arrives as nine blocks within a few
  ms. So each client's latency was anywhere in a 23 ms spread, different on
  every stream start and every re-arm. That explains why the two clients
  differed from each other by 7 ms.

**The design.** One output path, and a shared idea of when each block plays:

- The server no longer lets the library write I2S (`set_stream_reader(cb,
  false)`). It pushes each A2DP packet into its own ring and plays it
  through exactly the code a client uses (`driveRingI2S`). Its speaker is
  now as late as a client's (~90 ms against ~40 at first, 136 ms since the
  amendment below), with a buffer as deep as a client's against Bluetooth's
  own pauses. Its level controller also locks
  it to the source's clock, which the library's output never was.
- Every node reads its own output clock: an `i2s_write()` that had to wait
  returns just after a DMA buffer finished. At that moment the frames queued
  ahead of the next write are a whole number of buffers plus what the write
  put in its own, which the frame count gives exactly. Only the interrupt
  latency is unknown, and that is only ever late, so the earliest reading
  wins.
- Every packet carries `due`: how long after this frame left the radio its
  block plays on the sender's speaker. A client adds it to its receive time
  and has the server's play time on its own clock, late only by that packet's
  transit beyond the fastest. Keeping the earliest estimate from each window
  of ~97 packets removes the transit: the fast packets carry the truth, and
  a slow packet only ever makes the estimate later.
- A client aligns before it plays anything. It primes the DMA with silence
  until its clock is known, then lets go of (or waits out) exactly what
  separates its first frame from the server's schedule. After that it
  steers on the timing error rather than on its depth, using the D11
  controller with a fixed target and a 0.2 ms deadband. An error beyond
  4 ms (a server re-arming, a DMA running dry) is a jump, not a steer.

There is no clock-synchronisation protocol, no shared timebase and no extra
packets: two bytes per packet and a minimum. A bench source stamps a
schedule of its own (`BENCH_PLAY_DELAY_US`), so the automated harness
exercises the same path.

**What the hardware changed about it, the same day.** Four things, each
from a 600 s run behind the PC (`tools/btlisten/soak.py`):

- *An empty ring is not an underrun while the DMA still plays.* Windows
  pauses its A2DP stream for up to 54 ms. The ring holds half of the
  buffered audio and the DMA the other half, and re-arming as soon as the
  ring was empty threw the DMA's half away: 8–10 re-arms a minute per
  client, 2 on the server. Now a node waits for the next packet until its
  DMA has actually run dry.
- *The mesh's latency is the server's, so it has to suit the clients.* On
  the server's schedule a client's depth is whatever the server's leaves
  it. The 91 ms prefill left a client 15–48 ms of ring, often less than the
  28 ms a lost block's late copy needs (D13): 3,600 late copies per client
  and 2.2% holes. There is also the straddle: a block that spans two A2DP
  packets reaches a client up to a packet after the server had its first
  frames. `JITTER_PREFILL` went to 136 ms, and the same run gave 0.46%
  holes, lower than the 0.7% before sync.
- *A loss run longer than its silence fill slips the ring against the
  schedule*, and the client has to jump. 60 runs of 8+ in 600 s against an
  8-block fill made 20 jumps per client; the fill is 24 blocks now.
- *The server's own level controller moves everyone.* Its ring is fed a
  4 KB packet at a time and runs half a packet below a client's. The
  client's floor clamped its target up, and it inserted at the cap for half
  a minute, dragging every client later than they could follow. The server
  now has its own controller instance with a floor a packet lower
  (`SERVER_TARGET_BYTES`). Clients may steer at twice the server's rate
  (`SYNC_MAX_RATE`), since they have to follow its corrections plus their
  own drift.

**What was considered.** *Delaying the server's own output by a fixed
amount.* That was the `TODO.md` item as written, and it fixes only the first
cause. The 23 ms arming spread stays, and a server whose A2DP input runs
slower than its I2S drains its ring and slips a little more every time it
runs dry. *Pacing the server's sends to the stream's clock*, so that arrival
time means stream time: that adds up to a packet of latency and fights the
coexistence losses D13 is about. *NTP-style offset estimation from absolute
timestamps*: the same minimum filter plus a clock offset to track, and more
bytes on the wire. The relative stamp needs neither.

**What would change this:** a microphone showing a constant offset between
server and clients. `MESH_TRANSIT_MIN_US` (300 µs) is the least transit
assumed, and differences in DAC latency are not modelled at all; either
would call for a per-node trim. A source whose schedule moves often — each
move costs every client a jump. And a sample rate other than 44.1 kHz, which
changes every conversion in `lib/sync` along with everything else (`TODO.md`).

---

## D15 — Updates come over the home WiFi, in a boot of their own

**Decided:** 2026-09-30. **Status:** proven 2026-10-01 — on the bench
(`v0.2.0-64-g19f3314`: an update, a rollback, both ways of giving up) and on
the stereo node across the room (`v0.2.0-68-g5f444ca`: updated in 60 s at
−52 dBm, 240,425 packets received with none lost); 2026-10-02 on a C3, the
RISC-V build (`v0.2.0-85-gf1958d0`, 10 s at −64 dBm). `CHANGELOG.md`.

A node plugged into a stereo across the house has power and no cable to the
PC, so a new image has to arrive by radio. It arrives over the home network:
a node on USB broadcasts `U<room name>` over the mesh, the named node reboots
into **update mode**, joins the home WiFi with credentials it keeps in NVS,
and takes one image over HTTP. `tools/ota.ps1` drives all of it.

**Why a boot of its own.** A station follows its access point's channel, and
the router here is on channel 1 while the mesh is on 11 (`ESPNOW_CHANNEL`,
chosen to get away from it). A node cannot be in both, so update mode never
starts the mesh — or Bluetooth, the jitter buffer, anything that would claim
the radio or the heap. It is flagged in RTC memory and entered through a
restart, exactly like bench mode (D9), and cleared before it runs, so any reset
out of it lands back in normal mode. It ends in a restart whatever happens:
into the new image, or after five minutes with nothing uploaded.

**Why the request has a group of its own.** It could have been a beacon-style
packet in the audio group. But a node from before update mode takes the sender
lock *before* it checks a packet's length, so it would lock onto the relay and
ignore its real server until the next reboot. Stamped with `MESH_CONTROL_FORMAT`
instead of `MESH_WIRE_FORMAT`, the request is just another mesh's traffic to an
old node, and a mixed mesh during a rollout is exactly when requests are sent.

**Why two app slots, and rollback.** The image is written into the slot not
running (`min_spiffs.csv`, 1.875 MB each; the classic build is 1.60 MB), so
the old one is still bootable. The bootloader in this Arduino core is built with
rollback enabled, but the core marks a new image good before `setup()` runs,
which only catches an image that dies before main. `verifyRollbackLater()`
takes that decision back: an image is kept once it has run `OTA_CONFIRM_MS`
with its radio up, or when it answers the next update request — and until then
any reset boots the previous one. The radio condition is the point: an image
whose mesh never comes up can never hear another request, and keeping it would
make the USB cable the only way back.

**Why ESP-IDF, not the Arduino classes.** The first version used Arduino's
`WiFi`, `WebServer`, `Update` and `ESPmDNS`. It worked, and it cost **4,000
bytes of static DRAM on every boot**: mDNS's 1,460-byte packet buffer, lwIP's
1,184-byte DNS table, smartconfig's timers, the rest small. A Bluetooth server
has 21–23 KB free while it streams and forwards (`CHANGELOG.md`), and running
out of DRAM is what last crashed one. The IDF version — `esp_wifi`, `esp_netif`,
`esp_http_server`, `esp_ota_*`, the APIs the mesh already uses — costs 80 bytes
static, and its buffers are heap, taken only on the boot that updates. The image
is the raw POST body rather than a multipart form, which also leaves nothing to
parse.

**Why no mDNS, then.** It is the 1,460 bytes. The PC finds the node instead by
asking every address in its /24 for `GET /` and keeping the one that names
itself as the target — 1.6 s on this LAN, and it needs nothing from the router
or from Windows' name resolution. `-Ip` covers a wider network. The node also
sets its DHCP hostname to the lowercased room name, so the router's client list
shows it.

**Security.** For the five minutes update mode lasts, anyone on the LAN can
upload an image; the request that opens the window is unauthenticated
broadcast, honoured only inside the node's own mesh. The WiFi password is in
NVS in plain text, readable by anyone holding the board and a USB cable. For a
home LAN that is the same trust the router already extends; it is written down
so that it is a decision and not an accident.

**What was considered:** relaying the image itself over ESP-NOW, with the node
on USB as the proxy — no credentials, no channel change, works where the WiFi
does not reach, but 250-byte frames with acknowledgement and retry, and a
binary transfer through a serial line the relay also logs on: much more code,
worth it only if the WiFi does not reach a node. A second ESP32 wired to the
node's UART as a WiFi serial bridge — two boards at the stereo to do what
firmware can. A Raspberry Pi next to the node running esptool's RFC 2217
server — little software, but a Pi 1 has no WiFi. `espota` (ArduinoOTA) — needs
the board to connect back to the PC, which Windows' firewall blocks by default.

**Amended 2026-10-01 — silent.** Update mode first gave feedback with the
pairing tones: two beeps entering, rising when written, falling when it gave
up. With the startup sound on every reboot, one update and its read-back came
to six or seven tones — through a stereo, in a room somebody may be in, for an
operation driven from a PC that already reports every step. Update mode now
makes no sound, and the startup sound plays only on a power-on
(`ESP_RST_POWERON`, which the EN button and a USB flash also give), never on a
software restart. Pairing keeps its tones: there a person is standing at the
node, and the tones are their only console.

**What would change this:** a node out of reach of the home WiFi — then the
ESP-NOW relay above. A second person on the LAN who should not be able to flash
a speaker — then a shared secret with the request and the upload. A second
household mesh in range whose members matter — the same. Or a build outgrowing
1.875 MB: the build fails when it does, and the choice is then a smaller
image or bigger slots on boards with more than 4 MB of flash.

---

## D16 — Every command runs over the mesh, through the same dispatcher

**Decided:** 2026-10-02. **Status:** built; see `CHANGELOG.md` for what it
was measured against.

A node with no cable could be updated (D15) but not told anything: its volume
trim, the one setting a speaker in a living room most needs, was reachable only
over USB, and nothing could say which nodes were in the mesh at all, because a
client never transmitted.

Now any serial command runs on another node of the same mesh: `@<target>
<command>` typed on any node on USB, where the target is a room name, a MAC or
`*` for every node. The command travels in the control group D15 made for the
update request; the node named runs it through `commandRun()`, the very function
its own serial port calls; and what it printed comes back as replies, which the
asking node prints as `[@<name>] <line>`. `N` was added with it: one line per
node — mode, source, loss — so `@* N` is the mesh's topology, and
`tools/mesh.ps1` draws it.

**Why one dispatcher, and the output captured.** The alternative was a mesh
command per setting — a trim packet, a status packet — each with its own
encoder, handler and reply. Every command would then exist twice, the remote
half would fall behind the serial half, and a command added later would not be
reachable until somebody wrote its second half. Instead the serial front end
reads a letter and its argument and hands both to `commandRun()`; the mesh
hands it the same. The commands report the way they always have, by printing:
`DEBUG_SERIAL` is a thin `Console` over `Serial` that, while a remote command
runs, also copies what *that task* prints into a buffer on its stack. A command
added to the switch tomorrow works over the mesh with nothing more, and the
capture costs nothing between commands.

**Why clients transmit now.** Before this a client never sent anything, so that
audio had the air to itself. Replies are only ever answers: a few packets per
question, against 387 audio packets a second. Measured with a WROVER client of
a bench stream, both nodes asking `@* N` 20 times each in 105 s: 40 of 40 asks
answered by both, and the client received 40,747 packets with none lost and no
underrun. A question to `*` reaches every node at the same instant, and
broadcast is never retried, so each node waits a random part of
`MESH_REPLY_SPREAD_MS` before answering rather than all at once.

**Why an answer is sent again.** A question goes out `MESH_COMMAND_COPIES`
times; its answer went out once, and 2 single-packet answers in 10 were lost in
the minute after a reflash, while a phone was paging the asking WROVER to
reconnect — Bluetooth takes the radio from WiFi on these boards. The node that
answered now keeps the answer on the heap for as long as copies of the question
can still arrive, and answers each later copy again; the asking node prints
each part once (`meshReplyKey`). That is the retry the answer lacked, at no
static cost, and it needs no acknowledgement: a later copy is itself the sign
that the asker may not have heard. After it, 20 of 20 named and 10 of 10 `*`
questions were answered, each line printed once.

**Why the update request keeps its own format.** `@<name> U` does the same
thing, but the update request is how a node on older firmware gets the firmware
that understands commands — it cannot itself be a command. `U<name>` therefore
still sends D15's packet; it shares the outbox (one request at a time, repeated)
and the receive handler with commands, which also fixed the update request
being queued as a 248-byte packet into a queue of 252-byte items.

**What a command may not do.** Sent to `*`, nothing that reboots a node or
takes it off the mesh — `U b n c g p o w`: each would silence the house from
one keystroke, and only a node named by the person who meant it should do that.
`W` never travels, to anyone: the password would be broadcast in the clear. `@`
does not travel either; a node does not relay for another. And `U` sent over
the mesh is refused by a node serving a phone, as D15's request is.

Control traffic is now heard by a server too: a listing has to hear from the
node every client is locked to, and a server on USB has to hear the replies to
its own questions. It is checked before the server's early return in the
receive callback, and touches nothing the audio path reads.

**Why not Bluetooth Low Energy.** Every board here has BLE, and a phone could
talk to one node with it — but only to one. It shares the 2.4 GHz radio with
ESP-NOW, taking airtime from the audio, and costs RAM a streaming Bluetooth
server does not have (21–23 KB free, D15). If a phone app is ever wanted, BLE
to one node that is not streaming, which then asks the mesh exactly as a node on
USB does now, is the shape it would take; the commands would not change.

**Costs.** 288 bytes of static RAM on the classic build and about 4 KB of
flash. The asking node creates a ~2 KB queue for replies the first time it
asks; a node nobody types `@` into never does. The answering node holds its
last answer on the heap for half a second. No signal strength in `N`: the receive
callback on this core (IDF 4.4) carries none, and loss is what the audio cares
about.

**What would change this:** a command whose answer outgrows
`MESH_REPLY_CAPTURE` (768 bytes, four packets). A need to set something on many
nodes with certainty — today nothing is acknowledged, and a missing answer is
the only sign a command was not heard; asking again is the remedy, which a
per-node acknowledgement and retry would replace. A second person on the mesh
who should not be able to change settings — then a shared secret, as D15 would
need for the same reason. Or a core on IDF 5, which would put the RSSI of the
server's packets into `N`.
