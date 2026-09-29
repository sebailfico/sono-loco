# SonoLoco TODO

What is **open**. Completed work is in `CHANGELOG.md`, standing design choices
and their reasoning are in `docs/decisions.md`. Keep those three separate — this
file previously carried all of it and the open list got lost inside the done one.

## Current state (2026-09-14)

**The ESP-NOW mesh path is proven on hardware, and so is clock-drift
correction**: WROOM sourcing to four clients, 600 s, zero lost on the S3 and
C3 and drift held on all four. Full numbers in `CHANGELOG.md`. Reproduce with
`./tools/bench-mesh.ps1 -Duration 600 -Source COM8` (see `docs/bench-test.md`
for why not `-Flash`), and add `-NoDrift` for the uncorrected baseline.

The 41 host tests also pass on-device (`pio test -e esp32dev`): 23 for the
jitter buffer and sequence accounting, 18 for the clock-drift controller.

Boards on the bench: a WROOM (COM8), an ESP32-C3 (COM10), an ESP32-S3
(COM9) and, since 2026-09-14, two WROVER-Es (COM12, and WROVER2 with a DAC
— COM13 then, COM19 since 2026-09-28). DACs are wired on the WROOM, the C3
and WROVER2; the S3 and the
COM12 WROVER have none. The WROOM's auto-reset into download mode has become
unreliable (see "Bench" below); run the harness without `-Flash` and flash it
by hand with retries.

**Four clients at once is proven** — zero loss on the S3 and C3 over 600 s on
channel 11 with the house router busy on channel 1. What is *not* proven is a
building; see "Surviving the wild" below, which is the real list.

The Bluetooth server path works end to end, as of 2026-09-29: the PC
streaming into WROVER1, WROVER2 playing the mesh stream, 0.094% lost over
600 s where it had lost 12–24% (`CHANGELOG.md`). What is left of the loss, and
a phone on the new build, are under "Blocking" below. Both WROVERs moved USB
sockets again that day: WROVER1 (MAX98357A) on COM23, WROVER2 (TPA3116) on
COM11.

---

## Blocking

- [ ] **What is left of the mesh loss behind a Bluetooth server.** The 12–24%
      is gone: every frame now goes out twice at 12 Mbps, and 600 s behind a
      streaming server lost 0.094% (`CHANGELOG.md`, 2026-09-29). What remains
      comes in runs of 2 or more blocks, about one every 10 s, each a 9–40 ms
      hole of silence — audible as an occasional tick. In order of cost:
      1. **Concealment** ("Surviving the wild" below): fill a lost block from
         the one before it, faded, instead of zeros. Softens every hole the
         copies miss, whatever caused it.
      2. **Copies further apart.** Both copies go out back to back, so a
         burst that takes one takes both. Sending the second a few frames
         later would decorrelate them, but the client then sees a sequence
         number *behind* the last one, which `SeqTracker` treats as a resync
         today; it would need to accept a late copy and patch the silence it
         already queued for that block, while it is still in the ring.
      3. **ADPCM with the previous block in every packet** (the "Bandwidth"
         item): half today's bytes, and any single lost packet is recovered
         from the next one, 4.5 ms later rather than 0.3.
      A **two-chip server** (an A2DP chip handing PCM to a €2 C3 that only
      does ESP-NOW) is no longer needed for this, and stays the answer only
      if the coexistence cost grows again — with a phone, say, which has not
      been measured since the fix. Measure the same way: `listen.py --serial
      <server> --client <client>`, and `l` on the client for the run lengths.
      Also unexplained: the server's own output crackled once on 2026-09-28
      (27 holes/s, one per A2DP packet) and never again after a reboot. If
      it comes back, run `listen.py --serial` at once and compare the `a`
      window with the clean ones in `CHANGELOG.md`.
- [ ] **Finish the bench regression for 12 Mbps with two copies.** CLAUDE.md
      asks for a 600 s `bench-mesh.ps1` after any client-path change; the one
      run on 2026-09-29 was stopped at ~290 s (log flushed to 183 s). What it
      showed, WROVER2 sourcing to WROVER1: 220.5 pkt/s, `qfull=0`, 5 lost of
      40,286 with 93 blocks rescued by the copy, zero ovf/und/dup/rsy — no
      drift figure yet. **The bench tone is loud on any amp: ask before
      running it, and run it where nobody has to listen.
- [ ] **The S3's MAX98357A has never made a sound.** WROVER1's does, since
      2026-09-28 (`CHANGELOG.md`); its silence was a wiring mistake, not
      the board. The S3's I2S pins 4/5/6 have never driven a DAC. SD: WROVER1's clone
      plays with SD tied to 3.3 V (left channel); whether it has the pull-up
      that makes a floating SD work was not checked.
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
      sat 5 s in silence. Either keep a connectable (page-scan) BT on clients,
      at whatever that costs the radio, or document the two-step. Separately,
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
- [ ] **Solder a DAC to the S3** and pick its I2S pins — `config.h` hardcodes the
      WROOM/WROVER pins (26/25/22) for every board. Until then the S3 is verified
      only as far as "packets arrive and the buffer stays healthy", with no audio
      out.
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
- [ ] **Redundancy, so a lost frame is not a hole.** The only thing that
      recovers a broadcast frame nobody heard is having sent the data twice.
      Cheapest form: each packet carries its own block and the previous one
      (2× payload; with ADPCM 4:1 that is still half of today's bytes and a
      third of today's airtime at 6 Mbps). Any single lost packet is
      recovered from its successor with one packet of extra latency. Next
      form up: XOR parity every N packets, recovering one loss per group for
      1/N overhead. Measure on the bench with the monitor watching and the
      router deliberately loaded (a phone video is a repeatable enough
      "wild"): the counter that matters becomes `und`, not `lost`.
- [ ] **Concealment instead of silence.** Bursts longer than the redundancy
      window will still happen — 214 consecutive-ish packets in one second
      were seen at 1 Mbps. Repeat-and-fade the last block on a gap rather
      than zero-fill; the drift controller already knows how to insert.
      This is what stops a 20 ms hole being a click.
- [ ] **Buffer for bursts, and decide who waits.** A deeper jitter buffer
      rides out longer bursts at the cost of latency, and latency is only a
      problem relative to the server's own local playback (every client is
      delayed equally). Consider delaying the server's local output to match
      the clients — the "rooms will not sound alike" item under "Audio
      quality" is the same decision from the other side.
- [ ] **Smaller frames.** ADPCM (the "Bandwidth" item) is not just for BT
      coexistence any more: a 50-byte payload at 6 Mbps is a tenth of the
      airtime of today's 200 bytes at 1 Mbps. Every collision the mesh does
      not have is one it does not need to recover from.
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

### Bandwidth

- [ ] 44 KB/s of ESP-NOW while BT Classic shares the same radio is thin. IMA
      ADPCM (4:1, cheap) would bring it to ~11 KB/s. This is also what would buy
      back the headroom to revisit D5. **Measured 2026-09-14 evening: the
      radio is the binding constraint the moment BT streams** — see the first
      "Blocking" item. This said frame *count* was what mattered; measured on
      2026-09-29 it is frame *length*: the same 220 frames/s lost 12% at
      6 Mbps and 2% at 12, and doubling the count to 441/s (two copies) cost
      nothing. So ADPCM's value here is shorter frames, and bytes to spare for
      carrying the previous block in every packet.

      Half-measured. `./tools/bench-mesh.ps1 -Flash -Duration 300` (v0.2.0-5-
      g9c371f9, clean tree, WROOM on COM8 + ESP32-C3 on COM10) shows pure
      ESP-NOW has headroom at the current rate with **zero** BT contention:
      source queued 65,930 packets in 299.0 s at 220.5 pkt/s — exactly the
      expected rate — with `qfull=0 senderr=0 radiofail=0` the whole run; the
      client received 65,928 at 0.00% loss, zero ovf/und/dup/rsy. That rules
      out the radio saturating itself against its own traffic at 220.5 pkt/s.

      It answers nothing about the actual concern. Bench mode never starts
      Bluetooth — that's what lets a WROOM take part at all (D3) — so this
      measured zero airtime contention because there was none to measure.
      Whether BT Classic actually stealing airtime from ESP-NOW degrades the
      client is now answered: yes, ~20% of frames at the source. Numbers and
      the plan are under "Blocking".

### Audio quality

- [ ] The server plays 44.1 kHz stereo locally while clients get 22.05 kHz mono,
      so rooms will not sound alike. Decide whether that is acceptable or whether
      the server should downgrade its own output to match.

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
