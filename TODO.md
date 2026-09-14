# SonoLoco TODO

What is **open**. Completed work is in `CHANGELOG.md`, standing design choices
and their reasoning are in `docs/decisions.md`. Keep those three separate — this
file previously carried all of it and the open list got lost inside the done one.

## Current state (2026-08-26)

**The ESP-NOW mesh path is proven on hardware, and so is clock-drift
correction**, isolated pair by isolated pair: WROOM+C3 and WROOM+S3 each PASS
clean 600 s runs with zero lost/ovf/und/dup/rsy and drift held by correction.
Full numbers in `CHANGELOG.md` (v0.2.0) and today's re-measurements in the
"Multi-client reliability" item below. Reproduce with
`./tools/bench-mesh.ps1 -Flash -Duration 600`, and add `-NoDrift` for the
uncorrected baseline.

The 41 host tests also pass on-device (`pio test -e esp32dev`): 23 for the
jitter buffer and sequence accounting, 18 for the clock-drift controller.

Boards on the bench: a WROOM (COM8), an ESP32-C3 (COM10), an ESP32-S3
(COM9) and, since 2026-09-14, a WROVER-E (COM12) — the S3 is back; what looked like a hardware dropout was a leftover
on-device unit-test binary left flashed from an earlier `pio test -e esp32s3`
run, not a fault. Two of the three now have a DAC wired: the WROOM (was
already working) and, as of today, the C3 — bench mode's tone is confirmed
audible on its output. The S3 still has no DAC soldered.

**Running two clients at once is not yet proven** — see "Multi-client
reliability" below; each pair is clean alone.

Still unproven: the Bluetooth server path, i.e. real audio from a phone
forwarded to clients. The WROVER that can do it arrived 2026-09-14 and boots
into SERVER capable (after one coexistence fix, see `CHANGELOG.md`); the manual
walkthrough has not been run yet.

---

## Blocking

- [ ] **Run the manual Bluetooth walkthrough on the WROVER** (`docs/bench-test.md`,
      steps 2–3): phone → `SonoLoco-WROVER` → a client. This is the one path
      the project exists for and it has never carried audio. Watch `heap=` and
      `maxalloc=` on the SERVER status line while streaming: the node sits at
      **15.5 KB of internal DRAM free** in DISCOVERY with BT + WiFi up, before
      a phone has connected or the SBC decoder has allocated anything. If that
      goes to zero the fix is in the memory options (e.g.
      `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST`, not set in the Arduino core's
      sdkconfig) or the WiFi buffer counts in `config.h`, not in `main.cpp`.
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

### Multi-client reliability

- [ ] **Two simultaneous clients degrade in a way neither does alone.** First
      tested 2026-08-26, having never had three boards on the bench at once
      before: WROOM (COM8, source) + ESP32-C3 (COM10) + ESP32-S3 (COM9)
      together, 600 s, `-Flash -Source COM8`. Both clients came back WARN —
      C3: 26 lost, ovf=14, 4 re-arms, drift correction reading ~199 ppm; S3:
      327 lost, ovf=12, 4 re-arms, ~199 ppm. No brownouts or resets in the raw
      capture, so the boards themselves stayed up; this is a stream-health
      problem, not a crash.

      The ~199 ppm figure on *both* chips, nearly identical, is the tell — it
      does not match either board's known drift (C3 -57.7 ppm, S3 -30.5 ppm
      from earlier single-client runs) and two different crystals do not
      coincidentally drift at the same rate. Immediately re-run isolated on
      the same boards, same session, same dirty tree (v0.2.0-5-g9c371f9*):
      WROOM+S3 alone — PASS, 0.02% loss, 0 ovf/und/dup/rsy, corrected -19.7 ppm.
      WROOM+C3 alone — PASS, 0.00% loss, 0 ovf/und/dup/rsy, corrected -40.7 ppm
      (1 re-arm, 89% clean coverage). Both pairs are healthy alone; only the
      three-way combination is not.

      Not yet isolated: whether this is RF self-interference between two
      receiving radios placed close together, a limit in how broadcast
      reception behaves with more than one simultaneous listener, or an
      artifact of the harness/host polling three serial ports at once instead
      of two. Spreading the two client boards physically apart and re-running
      the three-way bench would distinguish RF proximity from the other two
      candidates. This matters beyond the bench: the product is multiple
      rooms listening at once, so if this is a real mesh limit rather than a
      bench artifact, it is more urgent than anything else in this file.

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
- [ ] **Decide whether each mesh should get its own WiFi channel.** It would cut
      the RF contention between two nearby meshes as well as the logical
      crosstalk, and the id is already the obvious thing to derive the channel
      from. The cost is discovery: a client can no longer sit on `ESPNOW_CHANNEL`
      and listen, it has to scan. Do this after "Multi-client reliability" above
      says whether radio proximity is a real limit — if two clients degrade each
      other in one room, the answer changes what this is worth.
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
      back the headroom to revisit D5. Do the bench test first — measure whether
      bandwidth is actually the binding constraint before optimising it.

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
      client is still open, and needs a WROVER sourcing real A2DP audio while
      ESP-NOW streams to a client at the same time. Same hardware gap as "Buy
      a WROVER" above — still blocking, still the only way to test this.

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
