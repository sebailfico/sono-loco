# SonoLoco TODO

What is **open**. Completed work is in `CHANGELOG.md`, standing design choices
and their reasoning are in `docs/decisions.md`. Keep those three separate — this
file previously carried all of it and the open list got lost inside the done one.

## Current state (2026-09-30)

**The ESP-NOW mesh path is proven on hardware, and so is clock-drift
correction**: WROOM sourcing to four clients, 600 s, zero lost on the S3 and
C3 and drift held on all four. Full numbers in `CHANGELOG.md`. Reproduce with
`./tools/bench-mesh.ps1 -Duration 600 -Source COM8` (see `docs/bench-test.md`
for why not `-Flash`), and add `-NoDrift` for the uncorrected baseline.

The 41 host tests also pass on-device (`pio test -e esp32dev`): 23 for the
jitter buffer and sequence accounting, 18 for the clock-drift controller.

Boards on the bench: a WROOM (COM8), an ESP32-C3 (COM10), an ESP32-S3
(COM9, native USB; MAX98357A since 2026-09-30) and two WROVER-Es — WROVER1
with a MAX98357A on COM20, WROVER2 with PCM5102 + TPA3116 on COM22 (they
move with every replug: check the MAC). DACs are wired on all of them but
the WROOM's is untested lately. The WROOM's auto-reset into download mode has
become unreliable (see "Bench" below); run the harness without `-Flash` and
flash it by hand with retries.

**Four clients at once is proven** — zero loss on the S3 and C3 over 600 s on
channel 11 with the house router busy on channel 1. What is *not* proven is a
building; see "Surviving the wild" below, which is the real list.

The Bluetooth server path works end to end: the PC streaming into WROVER1,
WROVER2 and the S3 both playing the mesh. Behind a streaming server 0.7% of
blocks are holes over 600 s (2026-09-30, redundancy 11 packets back, D13) —
each a 2.6 ms click, about three a second — and each client underran 7 times. What is left, and a phone on the new build,
are under "Blocking" below.

---

## Blocking

- [ ] **The server still drops 3–9% of its own frames.** Measured behind a
      streaming server on 2026-09-30 (`CHANGELOG.md`): every client loses the
      same packets, run for run — singles, and runs of 4–5 (10–13 ms) when the
      server's Bluetooth link has the radio — and the send callback reports
      them sent. Redundancy 11 packets back (D13) left 0.33% as holes in the
      60 s A/B and 0.7% over 600 s, when 6% of packets were lost singly — the
      floor for one copy per block is about the loss rate squared, and in those
      minutes distance 1 would have left ~0.5%. What would move it, cheapest
      first: **XOR of blocks n-1 and n-11** in place of the one old block —
      the same bytes, and a lone loss is rebuilt from the next packet while a
      run is rebuilt from 11 later (estimated ~0.2% on the 600 s run);
      **concealment** (below, "Surviving the wild") so the holes left are not
      clicks; a counter for patches refused because the silence already
      played, which says whether the client's buffer is too shallow for the
      distance; a **loss trace** — each client logging the seqs it missed — so
      every scheme and distance can be scored on one recording instead of on
      alternating minutes whose conditions swung 3× (the singles-only minutes
      favour distance 1, the run minutes 11); `t2` on top of distance 11 at
      twice the airtime; and a **two-chip server** (an A2DP chip handing PCM
      to a €2 C3 that only does ESP-NOW), which removes the cause. A phone as
      the source has not been measured since any of this.
      Also unexplained: the server's own output crackled once on 2026-09-28
      (27 holes/s, one per A2DP packet) and never again after a reboot. If
      it comes back, run `listen.py --serial` at once and compare the `a`
      window with the clean ones in `CHANGELOG.md`.
- [ ] **Does the S3 stall when nobody reads its USB serial?** Suspected, not
      measured. When the harness was killed mid-run on 2026-09-30, the S3's
      buffer jumped from ~8 KB to ~18 KB — 57 ms the output stopped consuming
      — and the drift controller spent minutes dropping it back. `Serial` on
      the S3/C3 is the native USB CDC, whose `write()` waits up to 100 ms for
      a host that is plugged in but not reading, and the S3 prints a status
      line every second from `loop()`, which also feeds I2S. A classic ESP32's
      UART never waits. Test: stream with COM9 closed, compare `und`/`jit` with
      the port read. Fix if so: `Serial.setTxTimeoutMs(0)` on those targets —
      a lost log line instead of a stall. A board on a phone charger is not
      affected (no host, the core drops the bytes).
- [ ] **Tones on every board, and loud enough for the amp they are on.** The
      user wants the startup tone on every node, DAC or not, BT or not — a
      client-only build plays nothing at boot today because the startup tone
      sits under `ENABLE_BLUETOOTH` on the old assumption that only BT nodes
      have a DAC. And `TONE_AMPLITUDE` 500 (-36 dBFS, chosen for the TPA's
      gain) is under a milliwatt into a MAX98357A at 9 dB. Plan: startup tone
      unconditional (it needs the tone I2S init, which is compiled on every
      build); `TONE_AMPLITUDE` behind `#ifndef` so `platformio.ini` can set
      `-DTONE_AMPLITUDE=4000` on the MAX98357A nodes (`esp32wrover`,
      `esp32s3`), the way I2S pins already can. A short "joined the mesh"
      tone on entering CLIENT would fit the same mechanism.
- [ ] **Measure whether modem sleep costs a BT node any ESP-NOW packets.** A
      node that runs Bluetooth now keeps the IDF default `WIFI_PS_MIN_MODEM`,
      because `WIFI_PS_NONE` aborts the coexistence layer (README gotcha). The
      comment in `setupESPNow()` says modem sleep makes reception miss
      packets, but that was never measured, and IDF documents modem sleep as
      engaging only while associated with an AP — which this mesh never is. The
      test: WROVER in *normal* mode (not bench — bench never starts BT, so it
      gets `PS_NONE` like everyone else) as a CLIENT of a bench source, 600 s,
      compare `lost`/`und` against the baseline. If it costs packets, the
      next thing to try is `esp_now_set_wake_window()`.
- [ ] **A per-node volume trim.** The user's ask, 2026-09-28: every node
      should be heard clearly whatever its amp and speaker — WROVER2's TPA3116
      is loud by 50%, WROVER1's MAX98357A is a small amp. Today the phone's
      volume is applied on the server before forwarding, so every room follows
      one slider and no node can differ. Proposal: a gain in dB per node, in
      NVS like the mesh name, set over serial (`v<dB>`), applied at that
      node's output — on a client in `driveClientI2S`, on the server to its
      local output only, after the forward is taken. The phone slider still
      moves every room together; the trim sets each room's offset. Worth
      deciding at the same time whether the mesh should carry pre-volume
      audio plus the volume value instead: at a low phone volume the forwarded
      16-bit stream has already lost bits the clients cannot get back.
- [ ] **A node playing as a client cannot be connected to.** Its Bluetooth
      is stopped in CLIENT, so a phone trying to take it over fails ("Couldn't
      connect", 2026-09-28) until the current server stops and the node has
      sat 5 s in silence. Keeping it connectable is now measured, and costly:
      a WROVER client whose Bluetooth kept page- and inquiry-scanning lost
      4.7% of a streaming server's packets beside an S3 losing 2.0%
      (2026-09-30), which is why CLIENT now switches the controller off. A
      slower scan (`esp_bt_gap_set_scan_mode` plus a long page-scan interval)
      might be affordable; otherwise document the two-step. Separately,
      Windows then also failed on the freshly rebooted node with nothing at
      all reaching it, and later connected fine; not understood.
- [ ] **Measure the jingle fix** (`563833a`). `listen.py --serial COM21
      --pre f --at 3.0:J` against `--at 3.0:j`, in a quiet room with nothing
      else playing (the one attempt had music under it), and the notes broken
      out of each recording: the old way should show each note in pieces with
      the test tone between them, the new way one piece per note and no test
      tone under the jingle.
- [ ] **The TPA3116 on WROVER2 is too hot for its speaker**: very loud by
      50% on the phone, which on the library's curve is already −17.5 dB.
      Fix it on the amp — the module's gain jumper, or a divider between DAC
      and amp input (10 k series, 3.3 k to ground ≈ −12 dB). Not with a
      volume ceiling in firmware: the server forwards to the mesh *after*
      the A2DP volume is applied, so a ceiling on one node would turn every
      room down.
- [ ] **Install a host compiler** so `pio test -e native` can run — there is no
      gcc/clang/MSVC on this machine, only the PlatformIO cross-toolchains.
      `winget install -e --id MSYS2.MSYS2` then `pacman -S
      mingw-w64-ucrt-x86_64-gcc`, and put `C:\msys64\ucrt64\bin` on PATH.
      Low priority now that the tests run on-device.
- [ ] **Set `board_build.arduino.memory_type = qio_opi` for the S3** so its 8 MB
      embedded PSRAM is actually usable. `esptool` reports the PSRAM, the
      firmware reports `psram=0`. Nothing needs it yet, so this is only about the
      logs being honest.

---

## Open design issues

### Timing / sync

- [ ] **Server and clients are not time-aligned.** The server plays through A2DP
      with its own 46 ms I2S ring (prefilled at every stream start) plus
      whatever the BT stack holds; clients play after ~137 ms (91 ms prefill
      plus 46 ms of I2S DMA).
      Adjacent rooms will slap-echo. The server needs to delay its own local
      playback to match.
- [ ] **Re-measure drift on the WROOM/S3 pair.** Correction is proven on the
      WROOM/C3 pair (v0.2.0: -57.7 ppm uncorrected, zero underruns corrected, the
      two measures agreeing within 1.4 ppm). The S3 measured -30.5 ppm against the
      same source in v0.1.0, so it is a different offset on the same controller
      and worth confirming once it is plugged back in — it was disconnected
      partway through the session and never came back.
- [ ] **Sample rate is assumed to be 44.1 kHz.** If A2DP negotiates 48 kHz the
      clients play at the wrong pitch. Read the actual rate from the sink and
      either follow it or resample.

### Surviving the wild

The product is a mesh of speakers in somebody's flat, in a building full of
other people's routers. The bench is one room with one router, and even that
was enough to take 10.7% of the packets off the weakest client. Everything
here follows from three measured facts (2026-09-14, `CHANGELOG.md`):

1. **Loss is other people's traffic, not the mesh.** Four clients on one
   broadcast are clean when the channel is quiet. `tools/airmon` on a spare
   WROOM showed the band empty except channel 1 with the house router at
   -54 dBm, and every burst of client loss lined up with a burst of
   undecodable frames at the monitor — a Wi-Fi 6 router streaming to a
   phone, which an ESP32 can only count. Same afternoon, mesh moved to
   channel 11, same router traffic on channel 1: S3 and C3 zero lost.
2. **Broadcast has no retransmission.** A frame lost in the air is a hole
   in the audio. Today that hole is silence, and 196 underruns in a run is
   what it sounds like.
3. **Airtime is the lever the mesh actually controls.** 1 → 6 Mbps cut the
   loss under the same interference 4.6× (D13). Shorter frames collide less.

- [ ] **Channel agility — necessary, not sufficient.** In a seven-storey
      building every channel carries something; the best a survey can do is
      pick the least loaded one, and it should: on this bench that was the
      difference between 837 lost and 0. At boot the source surveys (the
      survey in `tools/airmon/src/main.cpp` is the measurement half), picks
      the emptiest of 1/6/11 — or of all 13 — and announces it; clients scan
      for the mesh's beacon instead of sitting on `ESPNOW_CHANNEL`. Re-survey
      only between streams, never while audio flows. This subsumes the
      per-mesh-channel item under "Mesh isolation". **What it does not do:**
      make the least-loaded channel quiet. Frequency *hopping* was considered
      and rejected: with a known static interferer a hop schedule visits the
      bad channel 1/13 of the time, the receiver is deaf for about a
      millisecond per hop, a client that loses the schedule has to rescan,
      and a WROVER server is already hopping its BT radio under the
      coexistence arbiter. Hopping is for a band you cannot survey; this one
      can be surveyed.
- [ ] **Redundancy against somebody else's traffic.** The cheapest form is in
      (D13): every packet carries its own block and the one 11 packets back,
      and a client patches the late block over its silence. It was tuned
      against a Bluetooth server's own radio; how far it carries under a
      loaded router is unmeasured. Next form up: XOR parity every N packets,
      recovering one loss per group for 1/N overhead, or the previous *two*
      blocks. Measure on the bench with the monitor watching and the router
      deliberately loaded (a phone video is a repeatable enough "wild"): the
      counters that matter are `lost` (holes left) and `rec` (rebuilt).
- [ ] **Concealment instead of silence.** Bursts longer than the redundancy
      window will still happen — 214 consecutive-ish packets in one second
      were seen at 1 Mbps — and behind a Bluetooth server 0.7% of blocks are
      holes today, about three a second, each a 2.6 ms drop to zero. Repeat-and-
      fade the last block on a gap rather than zero-fill; the drift controller
      already knows how to insert. A hole that is later patched (D13) must not
      be faded twice. This is what stops a hole being a click.
- [ ] **Buffer for bursts, and decide who waits.** A deeper jitter buffer
      rides out longer bursts at the cost of latency, and latency is only a
      problem relative to the server's own local playback (every client is
      delayed equally). Consider delaying the server's local output to match
      the clients — the "not time-aligned" item under "Timing / sync" is the
      same decision from the other side.
- [ ] **Smaller frames.** ADPCM spent its 4:1 on stereo, full rate and a
      redundant block (D5, D13), not on short frames: a packet is 246 bytes,
      about 0.2 ms at 12 Mbps. Under heavy interference shorter would collide
      less — 114-frame blocks could be halved at the cost of twice the
      packets and headers, or a household could choose mono. Measure under
      load before trading audio for it.
- [ ] **Test in an actual building.** Take the boards and the monitor to the
      real flat. Survey first, then a 600 s run on the channel the survey
      picks, with `-NoDrift` off and the monitor logging beside it, and
      keep both logs. Until then every number in this file is one room, one
      router.
- [ ] **Range-test 12 Mbps.** `ESPNOW_PHY_RATE` went 1 → 6 Mbps on
      2026-09-14 (interference) and 6 → 12 on 2026-09-29 (a Bluetooth
      server's own radio), each on bench measurements at one metre. 12 Mbps
      is about 4 dB less sensitive than 6 and 9 dB less than 1, and the two
      copies of every frame buy some of that back. A flat is not one metre:
      walk a client into the next room and the one after, 600 s each at 12
      and at 6 (`R<Mbps>` switches a source at runtime), with `rec` and
      `lost` on the client. 2 Mbps is not the fallback any more — behind a
      streaming BT server every frame waited 20–50 ms for the radio and the
      client dropped out (D13).

### Mesh isolation

Separation itself is **done** — every packet carries a 16-bit mesh id and a
client drops anything that is not its own (D12, `CHANGELOG.md`). It compiles on
all three builds and the derivation is covered by host tests. What is left is
proof and polish.

- [ ] **Prove it with two meshes in the air.** Nothing here has been tested with
      an actual second household. The check to run, on the three boards on the
      bench: put the source and one client on `gcasa rossi`, leave the second
      client on the default, and run the harness. Expect the odd one out to
      report `rx=0` with `fgn=` climbing at ~220/s — that counter is the whole
      point, it is the difference between "correctly ignoring the neighbours"
      and "hearing nothing at all" — while the matched pair stays as clean as a
      one-mesh run: zero lost/ovf/und/dup/rsy. Then `g` the odd one back and
      watch it join. `bench-mesh.ps1` now warns when nodes disagree, so a
      mismatched run cannot be mistaken for a broken one.
- [ ] **Pairing has never been pressed.** Both halves are written and compile,
      and no finger has been on either. What to check, on a board with a DAC so
      the tones can be heard (the C3 or the WROOM):

      - A 3 s hold opens a window at each end — two beeps, and the matching
        `[MESH] offering` / `[MESH] listening` line on the console.
      - Offer on one, listen on the other: the listening node adopts within a
        second, plays the rising tone, and comes back streaming on the new mesh.
      - **The negative case, which is the whole reason there are two presses.**
        With no offer open, a listening node must sit through a foreign stream
        and report `listening closed, no offer heard` after 60 s, having joined
        nothing. If it joins anyway, the beacon check is not doing its job.
      - The adopted id survives a **power cut**, not merely a reset — NVS is the
        whole reason to prefer it over RTC memory.
      - The button on both a C3 and a classic board: GPIO 9 there, GPIO 0
        elsewhere, and only one of those has ever been reasoned about twice.
      - A beacon arriving at a node **already on that mesh** changes nothing and
        charges no `rsy` — it is dropped before the sequence tracker, and a
        regression there would be an audible re-arm caused by a node saying
        hello.
- [ ] **Decide whether an idle server should be discoverable at all.** Pairing
      needs the offering node to be reachable, and today it is: a beacon is sent
      whether or not music is playing. But a node only *streams* while audio
      flows, so "is my server working?" still has no answer until somebody plays
      something. A slow idle beacon — one a second, say, outside pairing too —
      would let a client show that its mesh exists. Weigh it against the radio
      time it costs and against giving a neighbour a constant signal to see.
- [ ] **Each mesh on its own channel** is now a corollary of channel agility
      under "Surviving the wild": once clients scan for their mesh's beacon,
      two meshes end up on different channels for free whenever the survey
      says so, and the id keeps them apart when it does not.
- [ ] **Provisioning without a serial console.** `g<name>` needs a USB cable and
      pairing needs physical access to a button; neither is what somebody with
      six speakers in four rooms wants. A phone app over BLE, or a temporary
      SoftAP with a captive page, would set the mesh name (and `ROOM_NAME`, and
      client-only mode) on a board already on the wall. Note that a BT-capable
      node running BLE alongside A2DP re-opens the coexistence question D3 is
      about, so this is not free on a WROVER.
- [ ] **Payload encryption, if privacy ever matters.** The mesh id keeps a
      neighbour's player out, not a neighbour's receiver: ESP-NOW encrypts only
      unicast frames (per-peer LMK), and this design is broadcast by
      construction, so anyone running modified firmware can still listen. The fix
      is encrypting the payload under a key derived from the mesh name, on a
      client that already runs I2S, drift correction and a radio — measure before
      believing it fits.

### Audio quality

- [ ] **Listen to real music through the mesh on the ADPCM build.** The format
      was chosen by ear on the PC (`tools/codec/abtest.py`, D5). On 2026-09-30
      20 s of music played through the S3's MAX98357A — "works even if
      cracky", which was the 2–3% of holes then, not the codec. Music from the PC or a phone into a
      server, a client beside it, and the question the A/B left open: is the
      noise ADPCM adds audible on these speakers? If it is, D5 says what next.
- [ ] **A stereo pair.** Every client now gets both channels. Two nodes in one
      room could play left and right -- a per-node channel setting beside `M`.
      Nothing asked for it yet.

### Housekeeping

- [ ] **Is `String` concatenation in `LOG_*` actually fragmenting the heap?**
      Half-measured, and the half that is done is the half that cannot answer it.

      Over 600 s of normal-mode client playback (54 String-built status lines,
      132,048 packets) free heap was **byte-identical start to end: 93,020 →
      93,020**. That looks conclusive and is not: `ESP.getFreeHeap()` is *total*
      free, and fragmentation shows up as the largest allocatable block shrinking
      while the total stays flat. It is blind to the failure it was meant to
      detect.

      `maxalloc=` (`ESP.getMaxAllocHeap()`) is now in both the normal status line
      and the bench telemetry, which is the number to watch. The confirming run
      has not been done. Do that before touching the logging: on the evidence so
      far this may well be a non-problem, and the `printf` conversion would be
      churn across every log call in the file.

      `./tools/test-client-only.ps1 -Client COM8 -Source COM10 -Duration 600`
      reports both figures.
- [ ] Connect/disconnect tones write into `I2S_NUM_0` while the A2DP task also
      owns it; sequence them properly instead of interleaving.
- [ ] Rename `esp32-code/` to something consistent with the project name.

---

## EMI / "frying" noise (hardware, unchanged)

- [ ] I2S wire lengths <10 cm, twist BCK/WS/DATA together
- [ ] 100nF + 10µF decoupling close to ESP32 VCC and PCM5102 VCC
- [ ] Single-point grounding across ESP32 / PCM5102 / TPA3116
- [ ] Ferrite beads on I2S lines if noise persists
- [ ] Separate 3.3V LDO for analog vs digital

---

## Roadmap (not started)

- [ ] ESPectre presence detection — auto-pause when a room is empty
- [ ] Dynamic volume based on presence
- [ ] Home Assistant integration
- [ ] OTA updates
