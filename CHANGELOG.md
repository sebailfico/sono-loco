# Changelog

What has actually changed, newest first. This used to live in `TODO.md`, where a
growing list of completed work crowded out the open one.

Entries here are the *record*; the reasoning behind the standing design choices
is in `docs/decisions.md`, and the traps worth not re-introducing are in
`docs/gotchas.md`.

## Versions

Each released section is headed with the version the firmware reports, which is
`git describe --tags --always --dirty=*` baked in at build time — a board says
`v0.1.0` when it is running exactly that tag, `v0.1.0-3-gabc1234` three commits
later, and appends `*` when it was built from a tree with uncommitted changes.
See D10.

That is what makes the measurements below worth keeping: a figure like "-30.5
ppm" is a claim about a specific build, and the version is what lets you rebuild
it. `tools/bench-mesh.ps1` records the version of every node in its log header
and warns when a board is running something other than the tree you are reading.
Numbers quoted from a `*` build are not reproducible and should not be recorded
here.

Tag when a claim becomes true on hardware, not on a calendar. `v0.1.0` is the
mesh working on real boards; the next tag will be whatever makes the next claim
in `TODO.md` false.

**The hashes changed on 2026-10-02.** The history was rewritten to take
personal data out of it (an email, Bluetooth addresses, a local path, the
boards' MACs), which gave the commits new hashes. Every version quoted here has
been moved to the new hash of the same commit; the `v0.2.0-N` part did not
change and still counts to it. A board flashed before then reports its old
hash until it is reflashed.

---

## Unreleased

- **The README is about using SonoLoco** (2026-10-02). The code layout and the
  gotchas moved to `docs/code-layout.md` and `docs/gotchas.md`, the gotchas
  grouped by the part of the code they are in. The commands are in three
  tables — everyday, measuring, Bluetooth server — with the mesh ones (`@`,
  `N`) beside the rest, and the two "without a cable" sections became one.
  Gone: quoted measurements that `CHANGELOG.md` already holds, and a "use the
  full path" note that no longer showed a path.

- **`pio test` no longer takes a C3 or an S3 off USB** (2026-10-02).
  PlatformIO's generated Unity glue ends a run on `Serial.end()`, and on these
  chips `Serial` is the chip's own USB port: the C3 dropped off USB as its
  tests passed, and on every power-up after, until held in download mode with
  BOOT. `test/unity_config.h` and `.cpp` replace the glue for every suite.
  Proven on the S3: 31 of 31 `test_mesh`, and COM9 still there afterwards.

- **Every command runs on any node over the mesh, and `N` lists the mesh**
  (2026-10-02, `v0.2.0-87-g3c2a833`, D16). `@<name|mac|*> <command>` on any
  node on USB runs the command there and prints each node's answer as
  `[@<name>] <line>`; `tools/mesh.ps1` does it from the PC and draws `@* N`
  as the star the mesh is. One dispatcher for the port and the air, and the
  answer is what the command printed, so no command is written twice. The
  first listing of the house: WROVER, WROVER2 and the stereo WROOM, all
  three trims read from the desk.
  - *Over the air, two WROVERs*: each command ran once on the node asked,
    though its question went out five times; an `r` line too long for one
    packet arrived in two
    and was joined; `@* U` and `W` refused at the asking node, and `U` to a
    serving node refused by it, over the mesh. Answers were lost 2 times in
    10 while a phone was paging the asking WROVER, which is why an answer now
    goes again for each later copy of its question: after that 20 of 20
    named and 10 of 10 `*` questions answered, each line printed once.
  - *While a stream plays*: a muted WROVER1 client of a WROVER2 bench source,
    both asking `@* N` 20 times each over 105 s — 40 of 40 answered by both,
    40,747 packets received, none lost, no underrun, `qfull=0`.
  - *Bench regression*, 600 s, `-Mute`, WROVER2 sourcing to WROVER1, on a
    mesh of their own (`logs/bench-20261002-232623.log`): 386.8 pkt/s,
    `qfull=0`; 231,931 blocks, **zero lost/ovf/und/dup/rsy**, `sjmp=0 dry=0`,
    `se` +248 µs; correction 7.5 ppm (198 drops) — `v0.2.0-76`'s 7.6 ppm
    (201) and +250 µs. Two boards only: the S3 and the C3 were not on USB.
    `test_mesh`: 30 of 30 on the C3, then 31 of 31 on WROVER1 with the
    resend's `meshReplyKey` test.
  - *The stereo WROOM, from `v0.2.0-68`* over WiFi with the new firmware
    relaying D15's request: 16.2 s at −62 dBm, kept. It had counted
    `fgn=293760` — the bench run's packets, heard and ignored, the other
    mesh doing its job: the run's tone never reached the stereo.
  - *Fixed on the way*: the update request was queued as a 248-byte packet
    into a queue of 252-byte items, reading four bytes past it. And the
    `Wait-ForLine` that `ota.ps1` shares with `mesh.ps1` (`tools/common.ps1`)
    matched a half-received line, as `bench-mesh.ps1`'s once did — it named a
    relay with no name.
  - Cost: 288 bytes of static RAM on the classic build, about 4 KB of flash.

- **Update mode works on the C3 too** (2026-10-02, `v0.2.0-85-gf1958d0`).
  The first update over the air on a RISC-V build: `ota.ps1 -Env esp32c3
  -Relay COM11`, WROVER relaying, from `app0` into `app1` in 10.0 s at
  −64 dBm, then read back and kept. Nothing needed changing — the C3's
  bootloader has rollback enabled like the classic one, and its 798 KB image
  is under half a slot. Before that it would not boot at all: the DAC's
  ground was on GPIO 9, which is BOOT (`docs/gotchas.md`).

- **By ear, through a stereo** (2026-10-01, `v0.2.0-78-g718591d` on the
  WROVERs and the S3, `v0.2.0-68-g5f444ca` on the WROOM). Streaming into
  WROVER2, with the stereo WROOM across the room as a client: "works
  incredibly well" — the first real listening to the ADPCM mesh (D5) on a
  proper speaker, and the WROOM's first sound at all. It played nothing at
  first while reporting a healthy stream (`ota.ps1 -Status`: `before=CLIENT
  rx=22769 lost=28 und=0`, −54 dBm): the PCM5102A's header had never been
  soldered. Also from the soak that evening: a streaming, forwarding server
  with update mode in the build had 23.4–25.4 KB of heap over 600 s, against
  the 21–23 KB D15 recorded before update mode — the 80 bytes it costs do not
  show.

- **Which packets a client missed, and what every redundancy scheme would
  have done with them** (2026-10-01, `b54ab85`, `ca6dfa4`, `bb7ca7e`). The
  counters say how much was lost, not which packets, and *which* decides what
  a scheme saves — a lone loss beside a run, two losses 11 apart. Comparing
  schemes live meant alternating minutes whose loss swung 3× (D13). Now a
  client with `L1` prints a bit per packet, once a second (~120 bytes,
  `lib/jitter/losstrace.h`, 7 tests); `soak.py --trace` keeps it in its log;
  `tools/btlisten/losstrace.py` replays the firmware's rebuild rules on it —
  copy at every distance, XOR of blocks 1 and d in the client's single pass,
  and that XOR with every parity kept, as a bound — and with two clients
  reports the loss they share, which never left the server.
  - *Checked against the boards*: over 120 s and two 600 s runs the trace's
    lost-run histogram matched each board's own `[LOSS]` line exactly (23,
    48; 69, 254; 123, 271), and copy at 11 rebuilt all of it, as `lost=0`
    said. Aligning two clients by PC time works: on the bench almost none
    of the loss is shared (5 of 69 and 254), so it is each receiver's own.
  - *Behind the streaming server*, 600 s, `v0.2.0-76-g52b1711`, every node
    muted (`logs/soak-20261001-190328.log`): the server lost 6.4% of its
    frames, 96–98% of them missed by both clients. Holes left, S3 / WROVER2:
    none 6.42 / 6.31%; copy 1 1.76 / 1.60%; **copy 11, today's default, 0.50
    / 0.49%**; copy 15, the best copy, 0.43 / 0.42%; **XOR 1+11 0.29 /
    0.28%; XOR 1+15 0.24 / 0.23%**; XOR 1+15 keeping every parity 0.16 /
    0.15%. The model's copy-at-11 figure for the S3, 1,145 holes, against
    the S3's own count over nearly the same window, 1,134. The live run itself
    was clean: no `und`, `dry` or `sjmp`, `late=0`.

- **Modem sleep costs a Bluetooth node nothing** (2026-10-01,
  `v0.2.0-72-gdce5402`). A node that runs Bluetooth keeps `WIFI_PS_MIN_MODEM`
  because `WIFI_PS_NONE` aborts the coexistence layer, and the code said
  that made reception miss packets, unmeasured. WROVER1 as a CLIENT of
  WROVER2's bench stream, the S3 beside it as the control, 600 s each, all
  muted (`logs/pstrace-20261001-*`):

  | WROVER1 | its WiFi power save | WROVER1 missed | S3 missed (PS_NONE) |
  |---|---|---|---|
  | normal mode | `MIN_MODEM` | **69** (0.030%) | 254 (0.111%) |
  | bench mode | `NONE` | 123 (0.053%) | 271 (0.117%) |

  All singles, all rebuilt, `lost=0 und=0` throughout. Modem sleep engages
  only while associated with an AP, which this mesh never is.

- **Concealment, and a volume trim per node** (2026-10-01, `b9f609e`).
  - *Concealment* (`z1`, `lib/jitter/conceal.h`, 6 tests): a lost block that
    was not rebuilt played zeroes, a step at each edge and a click whatever
    the music. Both neighbours are known when the hole is made — it is the
    packet after it that reveals it — so the fill is the previous block
    played backwards from its last frame, crossfaded into the next played
    backwards into its first: no step at either edge, the same spectrum. A
    run fades out, rests, fades in; a block patched later by its copy
    replaces only its own fill. Off by default (`MESH_CONCEAL` 0) until a
    microphone has compared it with zeroes.
  - *Trim* (`v<dB>`, `lib/jitter/gain.h`, 4 tests): the ask of 2026-09-28,
    every node heard clearly whatever its amp. A gain in dB per node, −40 to
    +12, in NVS, applied where mute is — after the mesh has its copy — so the
    phone's slider still moves every room together. Clips, never wraps.
    Set, clamped and read back across a reboot on the S3.
  - *The startup sound on every node* (`02f927b`), not only Bluetooth ones,
    at `TONE_AMPLITUDE` 4000 on the two MAX98357A nodes (500, for the
    TPA3116's gain, was under a milliwatt there). Not yet confirmed by ear;
    an S3 or C3 plays it only when plugged in, not when flashed.
  - *Bench regression, `v0.2.0-76-g52b1711`, concealment on in both
    clients*, 600 s, `-Mute`, WROVER2 sourcing to WROVER1 and the S3
    (`logs/bench-20261001-185056.log`): 386.8 pkt/s, `qfull=0`; 231,931 and
    231,916 blocks, **zero lost/ovf/und/dup/rsy**, `sjmp=0 dry=0`, `se`
    +250 and −465 µs. Corrections 7.6 ppm (201 drops) and −53.4 ppm (1,413
    inserts) — the baseline's 7.6 and −53.3. 62 of 62 `test_jitter` on
    WROVER2.
  - *Two harness fixes found on the way*: `bench-mesh.ps1 -Mute` toggled each
    client blind, and a node already in bench mode is not rebooted, so one
    still muted from the last run would have been unmuted for the whole run
    (`2b71d51`); and `Wait-ForLine` matched half-received lines, which
    reported WROVER1 as off the mesh with `espnow=` empty (`8f32be7`).

- **The stereo node, across the room: updated, and measured, in silence**
  (2026-10-01, `3418110`, `66327e2`, D15 amended). The WROOM
  `SonoLoco-Stereo` sits across the room on a phone charger with its PCM5102A
  into the stereo, and the stereo was off for all of this.
  - *Silent from now on.* One update and its read-back had played six or
    seven tones into whatever the node is plugged into: pairing beeps into
    update mode, a rising tone when written, the startup sound on every
    restart. Update mode makes no sound now, and the startup sound plays only
    on a power-on (`ESP_RST_POWERON` — the EN button and a USB flash too).
    Pairing keeps its tones.
  - *Measurable from the PC.* In update mode `GET /` adds the home WiFi's
    signal where the node stands and the stream counters it had when the
    request arrived, carried across the restart in RTC memory.
    `ota.ps1 -Status` asks, prints and sends it back, uploading nothing. A
    client never transmits; this is the only way to read one with no cable.
  - *Updated across the room* to `v0.2.0-68-g5f444ca` through WROVER1 as the
    relay: 60 s end to end, 1,608,080 bytes in 16.1 s, `app1` → `app0`,
    PASS. The home WiFi there: **−52 dBm** (−66 on the bench), and the router
    had moved itself to channel 2 since yesterday — still far from the
    mesh's 11.
  - *Received across the room*, with the bench tone streamed from WROVER2 for
    720 s (`bench-mesh.ps1 -Duration 720 -Mute -Source COM22 -Ports
    COM20,COM22`, `logs/bench-20261001-142107.log`) and `-Status` asked
    through the S3 at 620 s: **240,425 packets, 0 lost**, 0 ovf, und, dup,
    rsy, sjmp or dry. One packet in ten minutes went missing and its block
    was rebuilt from the redundancy (`rec=1`, `runs=1,0,…`). WROVER1, next to
    the PC, on the same stream: 278,162 in 720 s, 0 lost, 242 dropped = 7.6
    ppm as this morning.

- **Update mode: new firmware over the home WiFi, no cable** (2026-10-01,
  `cc63de0`, `c69ee4e`, `a6816cd`, D15). Asked for a WROOM that lives at a
  stereo's aux input with a phone charger and nothing else. `U<name>` on any
  node on USB broadcasts a request in a control group of its own; the named
  node reboots into update mode, joins the WiFi stored by `W`
  (`tools/ota-wifi.ps1`), and takes the image as the body of `POST /update`.
  Two app slots (`min_spiffs.csv`) and rollback: a new image is kept once it
  has run a minute with its radio up, or answers the next request.
  `tools/ota.ps1` builds, asks, finds the node on the LAN, uploads and reads
  the version back.

  The first version used Arduino's WiFi, WebServer, Update and mDNS classes
  and cost **4,000 bytes of static DRAM** on every boot — a fifth of what a
  streaming Bluetooth server has left. Rewritten on ESP-IDF directly it costs
  **80**. Classic image 1.60 of 1.875 MB.

  **On hardware, `v0.2.0-64-g19f3314`.** The WROOM, now `esp32stereo` /
  `SonoLoco-Stereo`, updated through WROVER1 as the relay:
  - *An update*, 54 s end to end: the request heard at once, the home WiFi
    joined in 7 s (channel 1, RSSI −66 on the bench), the node found by the
    LAN scan, 1,607,488 bytes in 13.3 s into `app1`. It booted on probation
    (`pending=1`), was kept the moment it answered the second request, and
    was back on the mesh.
  - *Rollback*: an image with `abort()` in `setup()`, sent to `app0`. It
    booted on probation, aborted, and 2 s later the bootloader had the
    previous image running, mesh up, reporting `rolledback=app0`.
  - *Giving up*, on WROVER1: no WiFi stored → back on the mesh in 3 s; a
    network that does not exist → 30 s of trying, back at 35 s.
  - *The mesh*, because the receive callback gained a branch:
    `bench-mesh.ps1 -Flash -Duration 600 -Mute -Source COM22`, WROVER2 to
    WROVER1 and the S3 (`logs/bench-20261001-132240.log`). Nothing lost,
    overflowed, underrun, duplicated or resynced, 386.8 pkt/s, `qfull=0`.
    Corrections: S3 1,405 inserted (2.34/s), WROVER1 201 dropped (0.34/s) —
    the sync build's 2.44 and 0.38. The stereo WROOM, same mesh, joined as a
    fourth client on its own, muted, and had `lost=0 und=0` at 439 s.
  - `test_mesh` on WROVER2: 18 of 18, the 6 new ones on update targeting
    included.

  Not measured: free heap on a streaming Bluetooth server. The static
  difference is 80 bytes and nothing new allocates in normal mode, but that
  is arithmetic, not a measurement.

- **Every node plays on the server's schedule, the server included**
  (2026-09-30, `38a7532`, `cb355de`, D14). Reported by ear first: with
  WROVER2 muted, WROVER1 and the S3 were "a clear echo". Measured with the new
  `tools/btlisten/sync.py` (clicks into WROVER1 from the PC, one node unmuted
  at a time, every board on `v0.2.0-51-g8dced93`): the S3 **51.0 ms** and
  WROVER2 **43.8 ms** behind WROVER1's own speaker, the server's reference
  steady to 0.4 ms across the run. Two causes. The server played through the
  A2DP library's 46 ms I2S ring, and a client through its 91 ms prefill. On
  top of that a client armed wherever in a 23 ms burst of blocks its ring
  happened to reach the prefill, which is why the two clients were 7 ms apart.

  The server now plays its own stream through its own ring and the client's
  output code (the library's I2S output is off). Every packet carries `due`,
  when its block plays on the server's speaker, counted from its sending.
  Each node reads its own output clock off its blocking DMA writes. A client
  keeps the earliest estimate of the server's schedule in each 250 ms window,
  aligns to it before it plays anything, and then steers on the timing error
  with the drift controller at a fixed target. A bench source stamps a
  schedule of its own, 90 ms after generation. Wire format `0xAD04`: an
  8-byte header, 248-byte packets, the same 386.8/s. `lib/sync` holds the
  arithmetic, and its 11 tests pass on a board. Writing them found one bug,
  in a test: an unsigned frame index that went "negative" and wrapped.

  Gone with the library's I2S output: `q` (its DMA depth), `J` (the jingle
  written alongside the library), `serverPrefill`, and the write-timing half
  of `a`. That window now reports the gaps between Bluetooth packets and the
  server's own ring (`jit`, `und`, `ovf`, `dry`). A jingle is written from
  `loop()` into the output `loop()` also feeds, so it cannot interleave, and
  the clients jump to the schedule that follows it. Telemetry gains `sync=`,
  `se=` (µs, + = late), `sjmp=` and `dry=`.

  **First on hardware, silent, 90 s, `v0.2.0-55-gff1392a` on all three**
  (`bench-mesh.ps1 -Duration 90 -Mute -Source COM22`,
  `logs/bench-20260930-155847.log`). WROVER2 sourced, WROVER1 and the S3
  were clients, and nothing was lost, overflowed, underrun, duplicated or
  resynced, at 386.9 pkt/s. Both clients armed on the source's schedule
  (`[SYNC] armed late=65153us` and `65430us`, each dropping ~2,880 frames of
  prefill) and held it with no jumps and no dry DMA. The error settled at
  `se=` −480 µs on the S3 (2.44 inserts/s) and +245 µs on WROVER1 (0.38
  drops/s), steady to about ±20 µs. That is where proportional control parks
  (deadband + rate/kp), on opposite sides for clocks drifting in opposite
  directions: 0.7 ms between the two clients, against 7 ms before and 44–51
  ms against the server. The harness flagged WROVER1's buffer as drifting;
  that came from a sample taken before it armed, since under sync the ring's
  level is not what is steered.

  **Behind the PC, three fixes, and the echo gone** (`e2a3b0a`, `24f417c`,
  `2fe7791`; D14's amendment). The same microphone test on the first sync
  build put both clients within 1.5 ms of the server. Four 600 s runs behind
  the real server followed (`tools/btlisten/soak.py`, new: every node muted,
  telemetry read every 10 s while the stream is open). Each found something:

  - *An empty ring re-armed while the DMA still played* (`e2a3b0a`): 8–10
    re-arms per client in 35 s, 2 on the server, against Windows' pauses of
    up to 54 ms. Now zero in every run since, on every node. The old
    firmware had 7 per client in 600 s.
  - *The 91 ms prefill left a client too shallow for its late copies*
    (`24f417c`). The ring held 15–48 ms, and a late copy needs 28 ms plus
    the straddle: 3,600 late copies per client and 2.2% holes. A loss run
    longer than the 8-block silence fill also slipped the ring against the
    schedule: 60 runs of 8+, and 19–22 jumps per client. `JITTER_PREFILL`
    16000 → 24000 (136 ms, the bench source's delay with it) and
    `MAX_GAP_FILL_PKTS` 8 → 24: `late=0`, holes
    **0.46%** (0.7% before sync), one jump each.
  - *The server's own level controller dragged the schedule* (`2fe7791`).
    Its packet-at-a-time ring ran under the client floor, the clamp raised
    its target, and it inserted at the cap for half a minute (~300 inserts):
    −2 ms on the clients and that one jump. Now it has its own instance with
    a floor a packet lower (`SERVER_TARGET_BYTES`), and clients may steer at
    20/s.

  **Final, `v0.2.0-59-g712e685` on all three:**
  - *Microphone* (`logs/sync-20260930-175830-synced-136ms`): S3 **+0.82 ms**,
    WROVER2 **+1.48 ms** from the server, the reference steady to 0.42 ms.
    The S3's click-to-click spread fell from ~3 ms to 0.07.
  - *600 s behind the PC* (`logs/soak-20260930-174707.log`): no `und`,
    `dry` or `sjmp` anywhere, `late=0`, `se` within −0.53…+0.35 ms on both
    clients. Holes 1.28% and 1.23%, in a minute when the server itself lost
    11.7% of its frames — twice the previous run's 6.3%, matched packet for
    packet on both clients, so the server's radio, not the sync. D13's floor
    of about loss² predicts that.
  - *Bench regression, 600 s, `-Mute`, WROVER2 sourcing*
    (`logs/bench-20260930-175913.log`): 386.8 pkt/s, `qfull=0`; 231,930 and
    231,528 blocks, **zero lost/ovf/und/dup/rsy**. On the schedule in every
    sample, `sjmp=0`, `dry=0`, `se` +225…+257 µs on WROVER1 and
    −490…−441 µs on the S3. WROVER pair −2.2 ppm by the log. Corrections
    +7.6 ppm (202 drops) and −53.3 ppm (1,408 inserts), as in the first sync
    run: a client on the schedule corrects its I2S against the source's
    nominal rate, where the level controller's 4.5 ms deadband had absorbed
    WROVER1's share. The buffers sit flat at ~16.5 KB, the deeper prefill.

- **Behind a Bluetooth server, measured for real: two client bugs, and the
  redundant block moved 11 packets back** (2026-09-30, `541f717` … `26691a0`).
  The ADPCM stream had not yet been measured behind a streaming server; the
  PC streaming into WROVER1, with WROVER2 and the S3 (now with a MAX98357A)
  as clients, found three things.

  *WROVER2 had never been a client at all.* It counted the server's packets
  in DISCOVERY and tried CLIENT every 5 s, and every attempt failed: `I2S
  install (client) failed: ESP_ERR_INVALID_STATE`. `m` turns the A2DP
  library's output off, and the library uninstalls its I2S driver in `end()`
  only while its output is on, so a muted node kept the driver (`541f717`
  worked around it; `8183ddc` replaced that with a mute that zeroes this
  node's samples after the mesh has them — volume 0 for one speaker — and
  never touches the library's output).
  The earlier WROVER2 measurements ran in client-only mode, where Bluetooth
  never starts.

  *A Bluetooth-capable client kept scanning.* `end(false)` leaves Bluedroid
  and the controller up, page- and inquiry-scanning, and coexistence gives
  those scans the radio: in the same 60 s WROVER2 lost 4.7% (1,076 blocks, 70
  runs of 8+) and the S3 2.0%. CLIENT now disables Bluedroid and the
  controller, keeping its memory (`b800b3e`); the two clients then lost 2.9%
  and 3.2%, and a WROVER client has 111 KB of heap instead of 44 KB. The
  first version also deinitialised Bluedroid, and the library, which
  remembers having initialised it, looped forever on "Failed to enable
  bluedroid" when the stream ended (`88a05b0` disables only). Two full
  client ⇄ speaker cycles checked.

  *The rest is the server's.* The two clients' loss-run histograms matched
  packet for packet (`1015,11,4,11,8,1,4,12` against `1009,11,4,10,8,1,4,12`),
  so the frames never left WROVER1 — the "reported as success" gotcha: singles
  at 3–9% of packets, and runs of 4–5 (10–13 ms) when its Bluetooth link has
  the radio. A block one packet back died in the same run. So each packet now
  carries the block `MESH_REDUNDANCY_DISTANCE` packets back, a client pushes
  silence for a missed block and remembers where (`lib/jitter/holes.h`), and
  a late block is written over its silence if that has not played yet
  (`JitterBuffer::patch`, 10 new tests; 37 pass on a board). The distance
  travels in bits 8–14 of `len`, so `D<n>` on the server moves every client;
  wire format `0xAD02`. Alternating 60 s runs, holes per client: distance 1
  735 and 70, distance 6 372 and 19, distance 11 61 and 164 — pooled 1.17%,
  0.57%, 0.33%. Conditions swung 3× between minutes (singles-only minutes
  favour 1, run minutes favour 11); 11 is the default (`26691a0`).

  Tried and left off: pacing the server's sends (`P<us>`, `ESPNOW_TX_PACE_US`
  0). An A2DP packet from Windows becomes nine mesh packets at once, sent in a
  5–9 ms burst; 2 ms spacing doubled the single losses and thinned the runs,
  holes 379/376 and 214/211 against 410/340 and 336/338 unpaced — noise.

  **600 s behind the streaming server, `v0.2.0-45-gdfa4df9`, silent** (every
  node muted, 30 loops of a 20 s music clip): 231,962 packets; S3 **1,699
  holes (0.73%)**, 14,100 rebuilt, 7 underruns; WROVER2 **1,571 (0.68%)**,
  14,248 rebuilt, 7 underruns. Runs: 14,188 singles, 219 of 2, 95 of 4, 25
  of 8+ — a singles-heavy stretch (6% of packets), where one copy per block
  has a floor and distance 1 would have left ~0.5%. The A2DP side: 43 pkt/s,
  gaps up to 53 ms, `qfull=0`. Next is in `TODO.md`: XOR of blocks n−1 and
  n−11 in the same bytes, and underruns — the client aims for 44 ms of
  buffer behind a server whose own input pauses for 53.

  **Bench regression, same build, 600 s, `-Mute`, WROVER2 sourcing to WROVER1
  and the S3: 386.8 pkt/s, `qfull=0`; 231,931 and 231,528 blocks, zero
  lost/ovf/und/dup/rsy.** WROVER pair −3.4 ppm (log), no corrections; S3
  −44.4 ppm against WROVER2, held by 1,171 inserts. The harness flagged one
  "re-arm" per client with no underrun: a single 2,056-byte step — one I2S
  DMA buffer taken late — over its 11 ms threshold. A re-arm refills the
  whole 91 ms prefill; the threshold is now 45 ms.

  Also: the S3's MAX98357A on pins 4/5/6 plays — the PC microphone heard the
  bench tone at −35 dBFS unmuted against −82 muted, toggling with `m`, and
  20 s of music ("works even if cracky"). Plug the S3's native USB port: on
  its CH343 port it flashes but prints nothing. `listen.py --client` takes
  several ports. The WROVERs moved to COM20 (WROVER1) and COM22 (WROVER2).

- **An S3 or C3 client no longer stalls its audio for a PC that is not
  reading its serial port** (`69cbdb7`). Native USB serial waited up to
  100 ms per write for a host that was plugged in but not reading, and
  `loop()`, which also feeds I2S, waited with it. Found when the harness was
  killed mid-run and the S3's buffer jumped by 57 ms. Measured, 60 s behind
  the Bluetooth server with the S3's port closed: **9 overflows and 472
  dropped frames before, none after**, WROVER2 beside it clean both times.
  4 KB transmit buffer, 5 ms timeout (not 0: the core decrements it before
  testing it, and 0 wraps to forever). A board on a charger was never
  affected — with no host the core drops the bytes.

- **The mesh carries 44.1 kHz stereo, as IMA ADPCM** (`04751ea`, D5). Until
  now a client played 22.05 kHz mono: nothing above ~11 kHz, the anti-alias
  FIR 8–10 dB down above 8 kHz, and no stereo. Chosen by ear first:
  `tools/codec/abtest.py` ran two 20 s excerpts through both paths, and ADPCM
  was "way better" (28–31 dB SNR; the added noise "pretty noisy" isolated,
  not a reason to stay). The server now encodes what A2DP gives it — no fold,
  no FIR — in 114-frame blocks that each carry their decoder state; a packet
  carries its block and the previous one (246 bytes, 386.8/s), so a lost
  packet is rebuilt from the next and counts in `rec`, not `lost`. The second
  copy of every frame is gone again (`ESPNOW_TX_COPIES` 1). The codec lives in
  `lib/adpcm`, pinned by golden vectors to the Python reference the listening
  test used; `test_drift` was ported to the new byte scale (same behaviour in
  time: bytes ×4, corrections per ppm ×2). All four suites pass on a board.

  The mesh id is XORed with `MESH_WIRE_FORMAT` on the wire, so a node still
  on the old firmware drops the new stream as a foreign mesh instead of
  playing ADPCM bytes as PCM — full-scale noise. The client ring (32 KB) is
  now allocated in PSRAM where there is PSRAM: the static array had sat in a
  WROVER server's internal DRAM even when unused (−7.8 KB static RAM). New:
  `M` mixes a client to mono for one speaker (set on WROVER1, whose MAX98357A
  plays one channel), `m` mutes any node's speaker, and `bench-mesh.ps1 -Mute`
  runs silent.

  Bench regression, `v0.2.0-36-g4c29989`, clean tree, 600 s, muted, WROVER2
  sourcing to WROVER1: **386.8 pkt/s as expected, `qfull=0`; 231,543 blocks,
  zero lost/ovf/und/dup/rsy**. Drift between these two WROVERs is small —
  −4.4 ± 1.2 ppm from the logs, +8.2 ppm from the buffer slope over the whole
  run — and the controller made no correction in 600 s. The harness reported
  two re-arms with `und=0`: its 500-byte "level jumped" threshold was in the
  old format's bytes, and one decoded packet is 456. Now scaled with the
  format. Not yet measured behind a Bluetooth server: the PC's Bluetooth was
  off.

- **Mesh audio behind a Bluetooth server no longer crackles.** A client
  playing a server's stream lost 12–24% of its packets, every loss a single
  frame. That was the crackle, one hole per lost frame, and it was the same
  with the client's own Bluetooth off (client-only mode). So the server was
  losing them to its own BT link, as the air monitor had said on 09-14. Two
  changes (`025af5c`): ESP-NOW frames go out at **12 Mbps** instead of 6, and
  **every frame is sent twice**. Measured with the PC streaming a 997 Hz tone
  into WROVER1 (MAX98357A, muted with `m`) and the mic next to WROVER2 (TPA3116)
  as the client, `v0.2.0-30`…`-33`, one Bluetooth session:

  | rate | copies | lost | mic: dips/s |
  |---|---|---|---|
  | 6 Mbps (before) | 1 | 12–24% | 22.8–24.4 |
  | 12 Mbps | 1 | 1.0–2.0% | 1.6–2.5 |
  | 24 Mbps | 1 | 4.0% | 6.6 |
  | 54 Mbps | 1 | 1.6% | 2.5 |
  | 6 Mbps | 2 | 0.5–1.3% | 0.7–1.8 |
  | **12 Mbps** | **2** | **0 in 15 s** | **0** |
  | 54 Mbps | 2 | 0 in 15 s | 0 |

  Then the shipped build (`v0.2.0-33-gc733208`) with the server's own speaker
  on, as in use: three 6 s tones at 0, 0.36 and 0.18 dips/s, 0–8 of ~3,000
  blocks lost; and **600 s at 0.094% lost** (126 of 133,875), zero
  underruns, overflows and resyncs. The copy stood in for 2,347 blocks whose
  original never arrived (`rec`). The server's local output stayed clean
  throughout: `late=0` over 606 s, longest gap 34 ms against its 46 ms ring,
  `qfull=0`. What is left comes in runs of 2 or more blocks, about one every
  10 s, which a second copy sent right behind the first cannot catch.
  Without Bluetooth (bench mode, WROVER2 sourcing to WROVER1, a 600 s run
  stopped at ~290 s and logged to 183 s): 220.5 pkt/s, `qfull=0`, 5 lost of
  40,286, 93 rescued by the copy, zero ovf/und/dup/rsy. Too short for drift;
  the full regression run is in `TODO.md`.

  Two things that did not help: the coexistence preference (`e0`, WiFi
  first: 332 lost against 338 at the default), and 2 Mbps, where every frame
  waited 20–50 ms for the radio and the client fell back to DISCOVERY.
  Airtime is about what it was: 441 frames/s at 12 Mbps against 220 at 6.
  Range at 12 Mbps is untested, and so was 6; see `TODO.md`.

  The copies carry `ESPNOW_LEN_REPEAT` (top bit of `len`), so a client counts
  *blocks*: `rx` still matches the source's `tx`, a copy of a block that
  arrived is not a `dup`, and the new `rec` counter is the blocks the copy
  saved. Three new `SeqTracker` tests; 26/26 on a board.

- **A server can be driven without anybody at the PC.** `k<mac>` makes a
  server dial an A2DP source it is bonded with, the way a headset reconnects
  to a phone, and Windows accepts. A reflash no longer costs the link:
  flash, `k<mac>`, carry on. Also new, all at runtime so one
  Bluetooth session can compare them: `V<0..127>` the A2DP volume (a board
  that dialled in starts at `VOLUME_DEFAULT`, 1 of 127, which forwards a mesh
  stream too quiet to hear), `m` mutes the server's own speaker, `t<n>` sets
  the copies, `R<Mbps>` the PHY rate, `e<n>` the coexistence preference,
  and `l` prints a client's histogram of lost-run lengths. `a` now also
  shows how long frames wait for the radio (`txlat`). `listen.py --client
  COMn` prints a client's counters for the recorded seconds.

- **WROVER1's MAX98357A plays — it was never broken.** Silent on two
  speakers while WROVER1 streamed as a server, with its I2S clock running
  and its mesh stream audible on WROVER2. At the amp's pins a meter read
  BCLK 0 V, LRC 3.3 V, DIN 0 V where running clocks read ~1.6 V: the signal
  wires were not on the GPIOs. Rewired, it plays. It also survived an earlier
  supply reversal (5 V and GND swapped, which browned out the ESP32). SD is
  tied to 3.3 V (left channel only; one speaker).

- **The server's own output no longer runs dry between Bluetooth packets.**
  The A2DP library played on its default I2S ring of 8 × 64 frames, 11.6 ms,
  half of one 23.2 ms packet from Windows. It is now 8 × 256, 46 ms
  (`SERVER_DMA_BUF_LEN`), filled with silence at the start of every stream —
  a ring deeper than a packet no longer fills itself, and without the prefill
  it would run just in time. Measured on WROVER2, `v0.2.0-22-gf56b808`, the
  PC streaming a tone, forwarding on, both depths in one Bluetooth session
  (`q64` / `q256` between runs):

  | ring | board: late | board: longest gap | mic: holes | tone length |
  |---|---|---|---|---|
  | 46 ms | 0, 0, 0 | 32.3, 35.6, 37.9 ms | 0, 0, 0 | 6.01 s |
  | 11.6 ms | 3, 6 | 31.9, 24.9 ms | 1 of 22 ms; 2 of up to 14 ms | 6.04, 6.03 s |

  The two instruments agree to the millisecond: a 31.9 ms gap on the old ring
  is 20.3 ms of zeros and the mic heard a 22 ms hole; 24.9 ms is 13.3 and it
  heard 14. On the new ring gaps as long pass silently. Cost: 6 KB of internal
  DRAM (23.2 KB free streaming and forwarding, from 29.4) and 46 ms of local
  latency, which the server needs anyway to meet its clients. A phone on the
  same build — the last thing to crash a server short of DRAM — connected and
  played "very fine": 50 packets/s of 3–4 KB (Windows sends 43 of 4 KB),
  late 0, longest gap 21 ms, 21.4–22.4 KB free while streaming and
  forwarding. Two TX-queue overflows (`qfull=2`) in that minute, i.e. two
  mesh frames dropped at the source; the queue is the mesh item's business.

  The board's gap count was fixed on the way (`b8273ae`): it had measured
  only the wait for the next packet, missing the up to 5 ms the forwarding
  callback spends in the same task, so it undercounted. And the first `q`
  froze the audio (`c02cc85`): the library leaves I2S running across a
  suspend and does not restart it on resume, so a driver swapped in stopped
  stayed stopped, and the BT task blocked in `i2s_write()` for good.

- **The Bluetooth half has a test signal, and the PC can listen to it.**
  `tools/btlisten/listen.py` makes the PC the A2DP source: it plays a 997 Hz
  tone into a paired server and records the speaker with the laptop's own
  microphone, then reports holes in the tone (>6 dB, 1 ms resolution),
  clicks, and the tone's pitch and length as recorded. A run through the
  laptop's speaker is the control: 997.00 Hz, 6.02 s, zero dips. Two things
  had to be found first: Windows' default capture path runs voice noise
  suppression that erases a steady sine and gates the rest to exact zeros
  (RAW mode fixes it), and exclusive mode on this Realtek driver returns
  65–82 k frames/s for a 48 k stream. The server side got instruments too
  (`8b49010`): `a` prints a window of A2DP timing — packets/s, packet size,
  and a histogram of how long the BT task waited between packets against the
  11.6 ms DMA ring — `f` toggles forwarding, `w` stops WiFi. `mon.py` logs the
  window every 2 s during real use.

  What it found on WROVER2 (COM19, `v0.2.0-18-gfc9512e` for everything after
  the first row):

  | | pitch | tone length | dips/s |
  |---|---|---|---|
  | laptop speaker (control) | 997.00 | 6.02 s | 0 |
  | WROVER2, forwarding on, before reflash | 987.14 | 6.55 s | **26.8**, ~3 ms, every 24–28 ms |
  | WiFi off (`w`) | 996.99 | 6.02 s | 0 |
  | after reboot, WiFi on, forwarding on / off / on, low volume | 997.00 / 996.97 / 996.76 | 6.0 s | 0.4 / 0.2 / 1.4 |
  | same at 40% volume | 996.93 / 996.91 / 996.95 | 6.0 s | 0.4 / 0.5 / 0.4 |

  The crackle was real — one hole per A2DP packet, and time inserted — and
  did not come back after a reboot in any condition, so it is not simply
  WiFi being on; what state produced it is open. The board's own view of
  clean play: Windows sends 4096-byte packets (1024 frames, 23.2 ms) at
  43.0/s, the BT task waits 5–10 ms between them, and forwarding runs at the
  full 220.6 frames/s. The margin against the DMA ring is thin — about nine
  waits of 10–27 ms in the first 8 s of playback, heard as "a bit crispy at
  first" — which is the next item in `TODO.md`.

  Separately, "very distorted above 50–60% volume" on two different speakers
  was the TPA3116's supply running below its 12 V. At 12 V it is fine.

- **A phone has streamed through a SonoLoco server for the first time — and
  the first thing it did was crash it.** Two connection attempts to WROVER2,
  same crash both times: `assert failed: hash_map_set (data != NULL)` out of
  `btu_start_timer` during L2CAP link setup on HCI connection-complete. An
  allocation returning NULL with 4 MB of PSRAM idle, because the BT
  controller and every FreeRTOS object live in internal DRAM only, and the
  node had 15.5 KB of it. Four changes, measured on the same board:

  | | before | after |
  |---|---|---|
  | free after WiFi init | 109,304 | 125,736 |
  | free after BT start | **15,564** | **43,264** |
  | largest free block | 14,324 | 25,588 |

  The WiFi TX "trim" had never trimmed anything — this core builds the driver
  with *static* TX buffers, and the code set the dynamic count, a field the
  driver never reads; `static_tx_buf_num` 8 → 2, AMPDU and CSI off. The BT
  controller is now brought up Classic-only with the BLE half released, the
  way the A2DP library itself does on non-Arduino builds. The jitter ring
  lives in PSRAM where there is PSRAM, and the TX queue is 16 deep (8 was
  tried and dropped 23 packets at an audio restart; 32 was 6.7 KB for a
  counter that had never moved).

  With that: a phone paired, audio started and stopped six
  times, the node went SERVER → DISCOVERY → SERVER across a disconnect and
  reconnect, connect and disconnect tones played, and the heap held at
  **28–32 KB free while streaming, 18–21 KB with ESP-NOW forwarding on top**.
  The SERVER status line now carries `tx=`, the count of frames handed to the
  radio, because without it a server that plays locally and one that also
  broadcasts print the same line.

  Then the measurement the "Bandwidth" item had waited a month for, and it
  is bad: with the BT radio streaming, **~215 frames/s handed to the radio,
  ~170/s on the air** at the monitor 30 cm away, **~153/s at the client with
  24% loss**, underruns every second, 144 buffer re-arms in 150 s. And the
  server's *own* output — the TPA on WROVER2 — crackled, while the clients'
  MAX98357A boards produced nothing at all (unverified hardware, first item
  after this one in `TODO.md`). That is BT/WiFi coexistence on one chip
  hurting both radios: BT short by 2.5%, audible locally; ESP-NOW short by a
  fifth, with the send callback calling every frame a success.
  The plan is the first item in `TODO.md`; the short version is fewer frames
  (ADPCM at 55/s) and a coexistence preference, and if those are not enough,
  a two-chip server.
- **The mesh is on channel 11, and there is an air monitor to say why.**
  `tools/airmon/` is a standalone sniffer for a spare classic ESP32 (COM15):
  promiscuous mode, one line a second — airtime, SonoLoco's own ESP-NOW frames
  and their RSSI, beacons by SSID, foreign data by transmitter, FCS-failed
  frames — and a 13-channel survey on `s`. `capture.py` logs it with PC time;
  `correlate.py` lays that against a bench log second by second.

  First survey: the whole band empty except channel 1, where the house router
  sits at -54 dBm. First correlated run (`bench-20260914-161403` against
  `air-20260914-161319`, mesh on channel 1 at 6 Mbps): client loss tracks
  the seconds the monitor saw hundreds of *undecodable* frames — a Wi-Fi 6
  router streaming HE frames to a phone, which an ESP32 sees as a preamble
  and a checksum failure. Correlation with FCS-failed frames +0.24..+0.38
  per client, with decodable data frames near zero: the interferer is
  traffic the mesh's radios cannot even read. Then `ESPNOW_CHANNEL` 1 → 11,
  same source, same clients, the monitor left on channel 1 to watch the
  router carry on: **S3 0 lost, C3 0 lost** over 132,069 packets, WROVERs
  11 and 5. Forty minutes earlier on channel 1 the same S3 and C3 had lost
  837 and 312.

  This closes the question of whether four clients can share one broadcast
  (yes) and opens the real one — a building where every channel is busy —
  under "Surviving the wild" in `TODO.md`. Frequency hopping was considered
  there and rejected in favour of survey-and-pick plus redundancy.
- **Four clients at once, and the multi-client mystery has a cause: channel 1.**
  Five boards on the bench for the first time (WROOM source, two WROVER-Es,
  S3, C3), 600 s each, all on one clean tree.

  `v0.2.0-8-ga830cbe`, 1 Mbps (the ESP-NOW default), WROOM sourcing 132,079
  packets at 220.5/s with clean counters — and the four receivers lost
  WROVER-1 **0**, WROVER-2 280 (0.21%), S3 2,955 (2.24%), C3 **14,079
  (10.7%)** with 196 underruns. The loss came in multi-second bursts at the
  same instants on every board (the C3 down to 11 of 220 packets in a second
  while WROVER-1 took all 220), the C3's own clock ticking evenly through
  them. Not the source, not a stall: something else on the air, and each
  board losing according to how well it hears the WROOM over it. A 206-byte
  frame at 1 Mbps is about 2.2 ms, and 220.5 of them a second is roughly
  half of channel 1 — the mesh had been running on the busiest channel there
  is at the most collision-prone rate there is.

  `v0.2.0-10-gbd7326c`, same boards, same source, **6 Mbps OFDM**
  (`ESPNOW_PHY_RATE`, D13), immediately after: C3 1,495 (1.13%), S3 1,493,
  WROVER-2 708, WROVER-1 86. Total loss 4.6× lower; and the C3 and S3 now
  lost *identical* counts every minute (193/194, 208/205, 202/202 …) — the
  same frames — because a shorter OFDM frame either survives for everyone or
  dies for everyone. Then, seven and a half minutes in, it stopped: the last
  two minutes were zero loss on all four clients, and a 180 s run straight
  after gave WROVERs 0, C3 and S3 the same 25 frames. Four clients on one
  broadcast is fine. The neighbourhood is not, at some hours.

  Drift, incidentally, is now measured for both WROVERs against the WROOM:
  correction held both flat at +18/+15 ppm (two 600 s runs agreeing within
  0.6 ppm), and against a C3 source at +44/+42 ppm. Ignore the drift columns
  on a lossy node — missing packets look like drift to the controller.
- **The bench harness no longer needs esptool to know what a board is.**
  Without `-Flash` it opens every port and takes the chip from the board's own
  `[BENCH] id` line; with `-Flash` it identifies with `--connect-attempts 20`,
  twice, and says "did not enter download mode" rather than "no firmware
  build for this chip" when nothing answered. The WROOM on COM8 started
  failing its auto-reset into download mode about three tries in four today
  and had been silently dropped from two runs.
- **The first WROVER is on the bench (COM12, WROVER-E, 4 MB PSRAM), and the
  server-capable boot works — after one fix.** The very first boot was a boot
  loop: `abort()` in `coex_core_enable`, from `esp_bt_controller_enable`. The
  IDF coexistence layer requires WiFi modem sleep while the BT controller is
  enabled, and `setupESPNow()` had been calling `esp_wifi_set_ps(WIFI_PS_NONE)`
  since the mesh was first written — code that no board had ever executed with
  Bluetooth about to start, because the WROOM bails before WiFi and the S3/C3
  have no BT. Moving the call after BT start aborts too (`pm_set_sleep_type`
  in the WiFi task), so it is not an ordering question: `WIFI_PS_NONE` is now
  set only on a boot that will never start Bluetooth. With that, the WROVER
  boots `SERVER capable`, brings up ESP-NOW and BT and holds in DISCOVERY.
  First number for the BT + WiFi heap question: **15.5 KB of internal DRAM
  free, largest block 14.3 KB** before a phone connects (`ESP.getFreeHeap()` is `MALLOC_CAP_INTERNAL`
  on this core, so the 4 MB of PSRAM is not in that figure). `maxalloc=` now
  appears on the SERVER and DISCOVERY status lines as well as CLIENT, since
  that is the node it matters on. Whether modem sleep costs a BT node any
  ESP-NOW packets is now measurable and is in `TODO.md`.
- **Two SonoLoco households in radio range no longer join each other's
  music.** Every ESP-NOW packet now carries a 16-bit mesh id and a client drops
  anything that is not its own, so which stream a client plays is no longer
  decided by whoever powered up first. The packet header goes from 4 bytes to 6
  (204 → 206 of the 250 ESP-NOW allows, no measurable bandwidth cost), and the
  id is compared **before** the first-sender lock — a neighbour's server that
  took the lock would leave a node deaf to its own household until it next fell
  back to DISCOVERY.

  The id is FNV-1a over a normalised mesh name, folded to 16 bits, in
  `lib/mesh/` with nine host tests: two nodes disagreeing about what "Casa
  Rossi" hashes to produces silence with nothing in the log, which is the one
  failure mode worth pinning on a host rather than chasing on a bench. Known
  names are pinned to known ids there, so changing the hash — which would split
  every deployed mesh silently — takes a deliberately failing test.

  Set it with `g<name>` over serial, or adopt a neighbouring mesh with `p` or a
  three-second hold of the BOOT button. Both persist in NVS, because a node that
  came back on the factory default after a power cut would silently rejoin
  whichever neighbour is still on it. The default `MESH_NAME` stays shared, so
  one household still flashes and it works; the second household presses a
  button. See D12, and D6 for what this amends.

  Dropped foreign packets are counted and reported as `fgn=` in the telemetry
  and status lines — the difference between "the neighbours are audible and
  correctly ignored" and "nothing is arriving at all", which are identical in
  every other number this firmware prints. `bench-mesh.ps1` reports each node's
  `mesh=` and warns when they disagree, since a mismatch otherwise looks exactly
  like a client out of range.

  **The wire format changed, so flash every node together.** A mixed mesh is
  not a degraded mesh, it is a broken one: an old receiver reads the new mesh id
  as a sequence number and the new sequence number as a payload length.

  Not yet measured on hardware with two meshes in the air, and the button has
  never been pressed — both are in `TODO.md` with the runs to do.
- **Pairing takes a press at each end, and says out loud what happened.** Hold
  the button on a node of the mesh being joined and it *offers* itself for 60 s;
  hold it on the node being moved and it *listens* and adopts. Which half a
  press performs follows the node role — a server-capable node offers, a speaker
  joins — so there is nothing for the user to choose, and both halves are on the
  serial console (`o` and `p`) for what the button cannot express.

  This closes a hole the first version had: adopting the first foreign *stream*
  heard meant a neighbour could capture a node by doing nothing more deliberate
  than playing music inside the window. An offer now has to be made by somebody
  standing at the other mesh, pressing its button, in the same minute.

  An offer is broadcast as a beacon: an audio packet with no payload and
  `MESH_BEACON_SEQ` in the sequence field, 5/s, through the existing transmit
  queue — so the wire format did not break twice and there is no second send
  path to keep correct. It is dropped before the sequence tracker on the way in,
  because a beacon reaching `SeqTracker` reads as a stream restart and would
  re-arm the jitter buffer audibly. Three host tests cover the predicate,
  including the one audio packet in 65536 whose sequence number is the magic.

  The tones are the acknowledgement: two beeps when a window opens, the rising
  three-note tone on joining, a falling one when a window closes empty. Pairing
  is done by somebody holding a button on a box with no screen, and without them
  the button is indistinguishable from one that does nothing. They play only in
  DISCOVERY — in CLIENT the jitter buffer is feeding I2S, and on a SERVER the
  A2DP task owns it.
- **A mesh client's DAC output is confirmed working for the first time, on the
  C3.** Every prior client verification was packets-and-counters only — a
  healthy jitter buffer says nothing about whether audio actually comes out.
  Wired per `config.h`'s provisional S3/C3 pin map (BCK/WS/DATA on GPIO 4/5/6)
  and confirmed by ear: bench mode's synthetic tone was audible on the C3's
  PCM5102 output. The S3 still has no DAC soldered.
- **Heap telemetry reports the largest allocatable block**, not just free heap.
  Free heap alone cannot answer the question `TODO.md` asks about `String`
  logging: fragmentation shows as the biggest block shrinking while the total
  stays flat. A 600 s run had free heap byte-identical end to end, which proves
  less than it appears to — hence `maxalloc=`, in both the bench line and the
  normal status line.
- **The normal status line carries the drift controller's state** (`tgt`, `ins`,
  `drp`). It was only visible in bench telemetry, i.e. in the one configuration
  that is not the shipping one.
- **A drift correction now requires that playback actually advanced.** `DROP`
  applied even when I2S accepted nothing, discarding a sample that was never
  played to correct drift that had not accrued. `INSERT` already required it.
- **A WROOM can be a mesh client.** Client-only mode, set with the serial command
  `c`: the node never starts Bluetooth, so the BT/WiFi coexistence problem that
  needs PSRAM never arises and `setupESPNow()` lets it have the radio. This is
  the D3 amendment finally reaching the shipping path — bench mode has been able
  to demonstrate the mechanism since v0.1.0, but bench mode is a test mode.

  Measured on a normal boot with ordinary status output, not in bench mode: a
  WROOM took **12,870 packets in 60 s, zero overflow, zero underrun**, jitter
  buffer holding 2,880 bytes, 93 KB of heap still free.
  `tools/test-client-only.ps1` reproduces it, and checks the things that would
  make a pass meaningless — that the node is *not* in bench mode, and that
  Bluetooth never started.

  Stored in NVS rather than RTC memory, unlike bench mode, because it is a
  setting: a node wired into a room comes back as what it was after a power cut.
  Bench mode stays volatile for the same reason inverted — a board left in a test
  mode by a power cut is a trap.

  Whether Bluetooth may start is now one predicate, `btAllowed()`, rather than a
  condition repeated at four call sites. One of those four restarted BT when a
  node left CLIENT, which on a client-only WROOM would have been the D3 crash
  arriving minutes after boot.

- **The bench tone is a lookup table now, and a C3 can source.** Generating it
  with `sinf` per sample capped an ESP32-C3 at 37.8 packets/s against the 220.5
  the stream needs, starving its client into 159 underruns in 90 s. The C3 has no
  FPU, and `2.0f * M_PI * BENCH_TONE_HZ * t` was worse than it looked: `M_PI` is
  a double, so the whole expression was evaluated in soft-float double. The radio
  was never involved — `qfull` and `senderr` were zero throughout.

  The table holds a whole number of tone periods so it wraps without a click:
  `sampleRate / gcd(sampleRate, toneHz)`, which is 2,205 samples and 4.4 KB at
  440 Hz / 22.05 kHz. A `static_assert` catches a tone frequency that would
  demand a 44 KB one. **Measured after: 223 pkt/s on the C3**, with a WROOM
  client taking 19,596 packets at 0.02% loss.

## v0.2.0 — 2026-08-20 — Clock-drift correction, measured

`b335d5a…77601a2`, tagged at `77601a2`.

**It holds.** WROOM source, ESP32-C3 client, 600 s each way, same boards and same
session, `v0.1.0-7-g64fc8e7`:

| | uncorrected (`-NoDrift`) | corrected |
|---|---|---|
| buffer slope | **-2.55 B/s (-57.7 ppm)** | flat, 1,616–2,272 B |
| underruns | 1 | **0** |
| corrections | — | 531 inserted, 0 dropped |
| final level | drained to 1,216 B and re-armed | 1,968 B |

The two measurements agree. The uncorrected drift, -57.7 ppm, comes from
regressing the buffer level over the longest clean segment. The corrected run's
steady-state correction rate over its last 300 s is 1.241 samples/s = **-56.3
ppm** — the same physical quantity arrived at from the other side, 1.4 ppm apart.
That is the useful property of closing this loop: the correction rate *becomes*
the drift measurement, because a corrected buffer has no slope left to regress.

Note this pair drifts at -58 ppm where the WROOM/S3 pair measured -30.5 ppm, on
the same source board. Which is the argument for a controller rather than a
constant, made by the hardware rather than by assertion.

- **`lib/drift/`** — proportional control on the smoothed jitter-buffer fill with
  a deadband; `driveClientI2S` applies its decisions by duplicating or dropping
  one mono sample at a batch boundary. At -58 ppm that is 1.24 edits/s. No
  resampler, nothing per-sample in the audio path. See D11.
- **The target is measured, not computed.** Ring occupancy plus DMA content is
  conserved at the prefill, but the DMA ring does not sit permanently full — it
  holds about 1,880 of its 2,048 bytes, so the natural ring level is ~2,250 where
  the arithmetic predicts 1,952. The controller now adopts the level it observes
  when its settle window closes (calibrated to 2,311 on the C3) and reports it as
  `tgt=`.
- **The gain is set by how deep the buffer must stay.** P control parks the level
  at `target - (deadband + rate/kp)`. At the original kp=0.005 a -58 ppm client
  parked at ~1,290 bytes — and the baseline run underran on a two-packet loss
  with 1,304 bytes showing a second earlier. kp 0.005→0.02 and deadband 400→200
  park the same client near 1,990. A test now asserts ≥1,500 bytes for any drift
  up to ±80 ppm.
- **A settle window** holds corrections off for 12 s after playback arms: the
  2,048-byte step as the DMA primes is not drift and must not be corrected.
- 18 drift tests, including hour-long closed-loop simulations whose model is
  validated against the recorded v0.1.0 slope before anything built on it is
  believed. 41 tests pass on-device.
- Correction is switchable at runtime (serial `d`, `-NoDrift` on the harness), so
  before and after come from the same crystals at the same temperature.

### Fixed: a client declared five seconds of silence mid-stream

A C3 client dropped to DISCOVERY twice in a 600 s run reporting `ESP-NOW silent
for 5s`, while its own receive counter advanced by 221 packets every second
throughout. `millis() - lastRxMs` is unsigned, and the receive callback runs on
the WiFi task: it writes a fresh timestamp between `loop()`'s read of `millis()`
and its read of `lastRxMs` often enough to matter at 220 packets/s. The
difference wraps to ~4.29e9 and clears any threshold.

    [BENCH] silence now=13822 last=13823 delta=4294967295 rx=14

Each false trip re-prefills the jitter buffer — audible, and it silently corrupts
any drift measurement taken across it, which is how it was noticed. Fixed with
the signed idiom already used for `clientRetryAfterMs`; the A2DP warmup gate had
the same shape and is fixed too. In the README gotcha list now.

### Fixed: the bench harness quoted drift across buffer re-arms

The whole-run regression reported **-7.6 ppm** for a client whose every clean
segment read about **-58 ppm** — it was averaging a drain against the step back
up when playback re-armed. It now regresses the longest uninterrupted segment
and says what fraction of the run that covered. A drift figure that is wrong by
8x is worse than no figure, because it is quotable.

### Also

- **ESP32-C3 supported as a client** — a third build, for the reason D4 allows
  one: a third instruction set. It needs `ARDUINO_USB_MODE=1`, which the S3's
  board definition supplies and the C3's does not.
- **I2S pins are per target.** GPIO 22–25 do not exist on an ESP32-S3 and 26 is a
  flash/PSRAM pin there; the classic WROOM map was being handed to every board.

### Commit-based versioning

- **Firmware version derived from git.** `esp32-code/scripts/version.py` runs as
  a PlatformIO pre-build step and defines `FW_VERSION` from
  `git describe --tags --always --dirty=*`. There is no version constant to
  maintain, because a hand-bumped one is wrong exactly when it matters — after
  someone forgets. See D10.
- The version appears in the boot banner and in the `[BENCH] id fw=…` identify
  line, so a running board can be matched to a commit without guessing.
- `tools/bench-mesh.ps1` reports the firmware version of every node, records it
  in the log header, and warns on the three ways it can be wrong: a board older
  than the tree, a build from a dirty tree, or nodes disagreeing with each other.
  Its log header now carries the full `git describe` string instead of a bare
  short sha.
- **`v0.1.0` tagged** at `22688a1` — the first hardware-proven mesh.

## v0.1.0 — 2026-08-19 — First hardware run of the mesh

`704b501…22688a1`, tagged at `22688a1`. This is the first tag, so it also
covers everything in the sections below — they are the history that led up to
it, not earlier releases.

**The ESP-NOW path works.** A WROOM sourcing and an ESP32-S3 playing: 26,279
packets over 120 s, zero lost, zero overflow, zero underrun, zero duplicates,
zero resyncs, source rate exactly 220.5 pkt/s, `qfull`/`senderr`/`radiofail` all
zero. The 23 unit tests also pass on-device.

- **Bench mode** added to the firmware: a runtime mode, entered by a serial
  command and a restart, in which a node never starts Bluetooth and can generate
  a synthetic 22.05 kHz stream straight into the ESP-NOW transmit path. This is
  what makes unattended testing possible — the normal SERVER role needs a phone.
  See D9. Single-character serial commands: `?` identify, `b`/`n` reboot into
  bench/normal, `s`/`x` start/stop sourcing, `r` report.
- **`tools/bench-mesh.ps1`** added: discovers every attached ESP32, identifies
  each by chip, flashes the matching firmware, drives the whole set through a
  streaming test and reports stream health and clock drift. No board limit.
- **Fixed: the client underran 24 times a second, permanently.** `JITTER_PREFILL`
  was 2000 bytes while the I2S DMA ring held 4096 bytes of mono, so every arming
  of the jitter buffer was swallowed whole and immediately ran dry. The audio was
  surviving on DMA buffering alone and the jitter buffer never approached its
  intended depth. Prefill raised to 4000, client DMA ring reduced to 4x256
  frames, and `static_assert`s now tie the two together. Underruns went from
  1423 in 60 s to 0 in 120 s.
- **Fixed: the ESP32-S3 was silent over USB.** Its board definition sets
  `ARDUINO_USB_MODE=1` but not `ARDUINO_USB_CDC_ON_BOOT=1`, so `Serial` went to
  GPIO43/44 while the board enumerates on native USB.
- **Fixed: bench mode never engaged.** The flag was `RTC_DATA_ATTR`, and
  `.rtc.data` is re-initialised from the image on every boot that runs the
  bootloader. Now `RTC_NOINIT_ATTR`.
- Unit tests build for hardware as well as the host, so `pio test -e esp32dev`
  works without a host compiler.
- Ports confirmed: COM8 is a WROOM (ESP32-D0WD-V3, no PSRAM), COM9 an ESP32-S3
  with 8 MB embedded PSRAM. The previous `platformio.ini` mapping had the S3 and
  the WROVER the wrong way round.
- **600 s drift baseline taken.** 132,069 packets, zero lost, zero overflow,
  zero underrun, zero duplicates, zero resyncs. Client buffer drains at
  1.34 B/s = -30.5 ppm, ~18 minutes to exhaustion. The estimate converged across
  45/120/600 s runs, and the log-derived clock measure — usable at this run
  length, +8.1 +/- 1.2 ppm for the WROOM — agrees in sign and magnitude.
- **D3 amended.** The WROOM's exclusion from the mesh is narrower than recorded:
  it cannot be a *server*, because that needs BT and WiFi together, but it runs
  ESP-NOW fine as a *client* with BT off. It sourced the entire test.

## 2026-08-19 — Structure and docs

`ca4887d…debcb26`

- Ring buffer and packet sequence accounting extracted from `main.cpp` into
  `lib/jitter/`, with host tests in `test/test_jitter/` and a `native`
  PlatformIO environment. This is the code that produced the two worst bugs in
  the project and it needs no hardware to exercise. See D7.
- `docs/decisions.md` added: why ESP-NOW rather than a second Bluetooth link,
  why the WROOM is excluded, why one binary covers two modules, and what would
  change each of those.
- `docs/bench-test.md` added: the two-board bring-up procedure with pass
  criteria, so it does not have to be re-derived when the boards are on the desk.
- `ROOM_NAME` moved from `config.h` to the per-board environments in
  `platformio.ini`. The source tree is now identical for every node, and nodes
  advertise distinguishable Bluetooth names. See D8.
- `TONE_SAMPLE_RATE` removed. It was an alias for `BT_SAMPLE_RATE` that the code
  still used under the old name, and `TODO.md` claimed a rename that had not
  happened — exactly the drift this documentation split is meant to stop.
- Status lists consolidated: `TODO.md` owns what is open, this file owns what is
  done, the README points at both instead of maintaining a third copy.
- Everything above plus the two audit passes below committed to git. `TODO.md`
  and `CLAUDE.md` had never been tracked at all.

## 2026-08-18 — Second audit pass

`bb0b2e2`, `df88798` — audited before the repo was tracked, committed after.

Compiles for all three environments. Not flashed.

- **The firmware did not compile at all** for either BT environment: the global
  `btStarted` collided with Arduino's `bool btStarted()` in `esp32-hal-bt.h`.
  Renamed to `btSinkStarted`.
- Duplicate, reordered and restarted-server packets no longer read as a
  65535-packet loss. `gap` is unsigned, so anything behind `lastSeq` used to
  charge ~65535 to `lost` and splice 800 bytes of bogus silence into the stream.
  Exact retransmits are now dropped, large jumps re-baseline the sequence.
- `resetRxState()` runs on *every* entry to DISCOVERY, not only when leaving
  CLIENT. A `senderLocked` left over from SERVER mode made a node ignore every
  future server permanently; a stale `rxActive` sent it into CLIENT mode on a
  stream that had stopped minutes earlier. A SERVER no longer locks a sender.
- Failed CLIENT entry backs off `CLIENT_RETRY_BACKOFF_MS` instead of retrying
  every loop pass, which thrashed BT stop/start while a server was broadcasting.
- `jAdvance()` rounds the I2S byte count down to whole stereo frames, so a
  partial write cannot shift the jitter buffer's 16-bit framing.
- `static_assert`s on `ESPNOW_PAYLOAD_SIZE` (≤ 246, even).
- Uncounted `jPushSilence` failures now increment the overflow counter.
- `wcfg.static_rx_buf_num` restored to the IDF default of 10. It had been cut to
  4 "because PSRAM handles the rest" — it does not: WiFi static RX buffers are
  DMA-capable internal DRAM and cannot live in PSRAM. A four-deep receiver
  against a continuous ~220 pkt/s stream was a likely source of unexplained
  `lost` counts. Costs ~10 KB of DRAM. `dynamic_tx_buf_num` stays at 4 — the send
  semaphore keeps one frame in flight.
- `esp32-code/README.md` deleted; its audio-path diagram moved into the main
  README, everything else duplicated it.
- `platformio.ini` collapsed from three build configs to two; the three
  environment names remain as port aliases. See D4.
- Removed a trap in the S3 section: its comment told you to uncomment a second
  `build_flags` line, which is a duplicate key in one INI section — a hard
  `DuplicateOptionError` that stops the whole project loading.
- Tuneables moved out of `main.cpp` into `config.h` per project convention.

## 2026-08-18 — First audit pass

- `i2s_write` partial writes handled (`i2sWriteAll`; the client advances its read
  pointer by the bytes DMA actually accepted instead of discarding the rest).
- Jitter buffer made block-atomic — overflow drops a whole packet instead of
  individual bytes, which used to desync 16-bit sample framing permanently.
- RX callback validates payload length against the received length, reads the
  header unaligned-safely, and forces an even byte count.
- Client locks onto the first sender MAC so two servers cannot interleave.
- Lost packets replaced with equivalent silence, capped at 4 packets.
- `a2dpSink.end(false)` instead of `end(true)` — a node can go CLIENT → SERVER
  again without a power cycle.
- ESP-NOW TX gated on the send-complete callback; error counters instead of
  per-packet `Serial` logging in the callbacks.
- 4-tap `[1 3 3 1]/8` FIR before 2:1 decimation. An earlier off-tree bench put
  the 20 kHz alias ~50 dB down versus 0 dB for naive decimation; that test file
  is not in this repo, so treat the number as unverified.
- `esp_netif_init` / default event loop / `WIFI_PS_NONE` added to WiFi bring-up.
- WROVER PlatformIO environment added.

## Earlier

`8e044fd…0da167c`

Bluetooth A2DP sink, I2S output to the PCM5102, notification sounds and basic
volume control. This part has run on hardware.
