# SonoLoco TODO

What is **open**. Completed work is in `CHANGELOG.md`, standing design choices
and their reasoning are in `docs/decisions.md`. Keep those three separate — this
file previously carried all of it and the open list got lost inside the done one.

## Current state (2026-08-19)

**The ESP-NOW mesh path is proven on hardware.** A WROOM sourcing and an
ESP32-S3 playing: 132,069 packets over 600 s, zero lost, zero overflow, zero
underrun, zero duplicates, zero resyncs, source rate exactly 220.5 pkt/s.
Reproduce with `./tools/bench-mesh.ps1 -Flash -Duration 600`.

The 23 host tests also pass on-device (`pio test -e esp32dev`).

Still unproven: the Bluetooth server path, i.e. real audio from a phone
forwarded to clients. That needs hardware nobody here has yet.

---

## Blocking

- [ ] **Buy a WROVER.** It is the only module that can be a server (BT Classic +
      PSRAM), and without one the A2DP half of the system cannot be tested at
      all. The WROOM on the bench has no PSRAM; the S3 has no BT Classic.
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
- [ ] **No clock-drift correction.** Measured, not predicted. 600 s baseline
      (2026-08-19, WROOM source -> S3 client): the client's jitter buffer drains
      at **1.34 bytes/s, -30.5 ppm**, emptying it in about 18 minutes. Converged
      across run lengths (45 s: -42.5 ppm, 120 s: -27.2 ppm, 600 s: -30.5 ppm)
      and corroborated by the independent log-clock measure, which put the WROOM
      itself at +8.1 +/- 1.2 ppm against the PC.

      The fix is undemanding: 30 ppm at 22.05 kHz is 0.66 samples/s, so
      duplicating one mono sample about every 1.5 s cancels it. Well below
      audibility, no resampling required.

      Make it **adaptive**, not a -30 ppm constant: this is one pair of crystals
      at one temperature, and a third board will have its own offset. Sample the
      buffer fill every few seconds and nudge toward the target depth.
      `tools/bench-mesh.ps1` measures the result.
- [ ] **Sample rate is assumed to be 44.1 kHz.** If A2DP negotiates 48 kHz the
      clients play at the wrong pitch. Read the actual rate from the sink and
      either follow it or resample.

### Bandwidth

- [ ] 44 KB/s of ESP-NOW while BT Classic shares the same radio is thin. IMA
      ADPCM (4:1, cheap) would bring it to ~11 KB/s. This is also what would buy
      back the headroom to revisit D5. Do the bench test first — measure whether
      bandwidth is actually the binding constraint before optimising it.

### Audio quality

- [ ] The server plays 44.1 kHz stereo locally while clients get 22.05 kHz mono,
      so rooms will not sound alike. Decide whether that is acceptable or whether
      the server should downgrade its own output to match.

### Hardware reach

- [ ] **Expose a client-only runtime mode**, so a WROOM can be a real client
      node. Bench mode already proves a WROOM runs ESP-NOW fine as long as
      Bluetooth is never started — it sourced the whole 600 s test. The firmware
      just has no way to say "be a client, never a server" outside bench mode.
      See the D3 amendment in `docs/decisions.md`.

### Housekeeping

- [ ] `String` concatenation in every `LOG_*` call fragments the heap — switch to
      `printf`-style. Watch `heap=` in the bench test's five-minute run to see
      whether this is real or theoretical.
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
