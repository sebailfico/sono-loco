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
(COM9) and, since 2026-09-14, two WROVER-Es (COM12, and COM13 with a DAC).
DACs are wired on the WROOM, the C3 and the COM13 WROVER; the S3 and the
COM12 WROVER have none. The WROOM's auto-reset into download mode has become
unreliable (see "Bench" below); run the harness without `-Flash` and flash it
by hand with retries.

**Four clients at once is proven** — zero loss on the S3 and C3 over 600 s on
channel 11 with the house router busy on channel 1. What is *not* proven is a
building; see "Surviving the wild" below, which is the real list.

Still unproven: the Bluetooth server path, i.e. real audio from a phone
forwarded to clients. The WROVER that can do it arrived 2026-09-14 and boots
into SERVER capable (after one coexistence fix, see `CHANGELOG.md`); the manual
walkthrough has not been run yet.

---

## Blocking

- [ ] **A server streaming over Bluetooth loses a fifth of its ESP-NOW frames
      at the radio — and its own local playback crackles.** This is the whole
      product's path, first exercised on 2026-09-14 evening (phone → WROVER2
      → WROVER1 with a MAX98357A). **The crackle was heard on WROVER2's own
      TPA output**, i.e. the BT side is losing frames; the clients' MAX98357A
      boards produced nothing at all (see the next item), so the 24% mesh
      loss below has not been heard yet. Three logs recorded together (`logs/server-20260914-231730-COM13.log`, `client-…-COM12.log`, `air-…-ch11.log`):
      - server: `tx=` climbing **~215/s**, `senderr=0 radiofail=0` — the
        driver reports every frame sent. Also 2.5% below the 220.5/s that
        44.1 kHz implies, so the A2DP side is losing a little too.
      - air monitor on channel 11, 30 cm away at -24 dBm: **~170
        ESP-NOW frames/s**. It saw 219–221 of 220.5 from a bench source that
        afternoon, so it is not the monitor.
      - client: **rx ~153/s, `lost` 24%**, `und` every second, "Jitter buffer
        ready — starting I2S" 144 times in 150 s. A gap every few packets is
        the crackle; a 90 ms prefill after each re-arm is the lag.
      Nothing else changed between the clean 0-loss channel-11 bench run and
      this one except that the source's BT radio was streaming A2DP. That is
      the BT/WiFi coexistence cost the "Bandwidth" item said needed a WROVER
      to measure: the arbiter lets BT have the radio, WiFi frames get aborted
      or mangled, and the ESP-NOW send callback still reports success for a
      broadcast. Things to try, cheapest first, each measured the same way
      (server `tx=`, monitor `espnow=`, client `rx=`/`lost=`):
      1. `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` before BT starts
         (IDF 4.4 API). Expect fewer lost ESP-NOW frames and more lost A2DP
         frames; the server's local output will say whether the phone side
         still holds. `ESP_COEX_PREFER_BALANCE` is today's default.
      2. **Fewer frames.** The arbiter cost is per WiFi transmission, not per
         byte: 250-byte payloads are 176/s instead of 220; IMA ADPCM at
         today's 22.05 k mono is **55/s** — four times fewer radio hand-offs
         for the same audio. This moves the ADPCM item from "nice" to "the
         fix", and it is `lib/`-shaped with host tests. Do mono ADPCM before
         stereo ADPCM: stereo at 44.1 k puts the count straight back to 220.
      3. If neither is enough: a **two-chip server** — one ESP32 is the A2DP
         sink and plays locally, hands PCM over I2S to a second chip (a C3
         is €2) that only does ESP-NOW. No coexistence at all, at the cost of
         one module per server. Record the decision in `docs/decisions.md`
         if it comes to that; D3 already says what would change it.
      The crackle being local means the coexistence hurts both directions:
      BT loses ~2.5% of its frames (a gap every ~40 ms — a crackle), ESP-NOW
      loses ~20%. So a coexistence *preference* (1.) only moves the damage
      between the two; fewer WiFi transmissions (2.) or two chips (3.) are
      the real candidates. **First test tomorrow, before any of that:** pair
      the phone to `SonoLoco-WROOM` (COM8, default mode — no PSRAM, so WiFi
      never starts) and listen. Clean there and crackly on WROVER2 pins the
      crackle on coexistence, not on the WROVER's PSRAM cache workaround or
      the forwarding callback in the BT task. Crackly on both means the
      problem predates the mesh. Needs the phone: notify.
- [ ] **The MAX98357A boards are silent.** WROVER1 (COM12) and the S3
      (COM9) are wired to them. In the last window WROVER1 was receiving and
      driving I2S (144 re-arms, 24% loss) and **no sound came out** — the
      user confirmed it. So either the wiring (SD floating is right for
      mono; VIN on 5 V; GAIN floating), the power, or the I2S signal never
      reached the amp. Cheapest proof, no phone needed:
      bench mode, WROVER2 as source (`-Source COM13`), the 6000-amplitude
      tone is unmissable. The S3's I2S pins 4/5/6 have never driven a DAC.
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
- [ ] **Prepare a known test audio sample to stream over Bluetooth into the
      server board**, instead of testing the still-unproven BT path against
      whatever happens to be on someone's phone. Bench mode already solved
      this for the ESP-NOW side with a precomputed tone (D9); nothing
      equivalent exists for the BT-ingest half, so a manual walkthrough run
      today would not be reproducible or comparable across runs. Needs the
      WROVER above to actually play through, but the sample itself can be
      made now.
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
      with tens of ms latency; clients play after ~137 ms (91 ms prefill plus
      46 ms of I2S DMA).
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
      hometown flat. Survey first, then a 600 s run on the channel the survey
      picks, with `-NoDrift` off and the monitor logging beside it, and
      keep both logs. Until then every number in this file is one room, one
      router.
- [ ] **Range-test 6 Mbps against 1 Mbps.** `ESPNOW_PHY_RATE` is now 6 Mbps
      OFDM (D13) on the strength of one back-to-back comparison under
      interference: total loss 4.6× lower, worst board 9× better, but the two
      WROVERs that were clean at 1 Mbps picked up 86 and 708 lost — the OFDM
      sensitivity cost is real and was measured at one metre. A flat is not
      one metre. Walk a client into the next room and the one after, at each
      rate, 600 s each, before trusting either number. If 6 Mbps loses at
      range, 2 Mbps is the next thing to try, not a return to 1.

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
      "Blocking" item. What matters is frame *count*, so ADPCM's 55 frames/s
      is the point, not its bytes.

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
