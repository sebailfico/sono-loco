# SonoLoco TODO

What is **open**. Completed work is in `CHANGELOG.md`, standing design choices
and their reasoning are in `docs/decisions.md`. Keep those three separate — this
file previously carried all of it and the open list got lost inside the done one.

## Current state (2026-08-20)

**The ESP-NOW mesh path is proven on hardware, and so is clock-drift
correction.** A WROOM sourcing and an ESP32-C3 playing, 600 s: zero underruns,
zero overflow, buffer held flat against a -57.7 ppm offset that drains it to an
underrun without correction. Full numbers in `CHANGELOG.md` (v0.2.0). Reproduce
with `./tools/bench-mesh.ps1 -Flash -Duration 600`, and add `-NoDrift` for the
uncorrected baseline.

The 41 host tests also pass on-device (`pio test -e esp32dev`): 23 for the
jitter buffer and sequence accounting, 18 for the clock-drift controller.

Boards on the bench: a WROOM (COM8) and an ESP32-C3 (COM10). The S3 dropped off
USB partway through 2026-08-20 and has not been seen since.

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
- [ ] **Re-measure drift on the WROOM/S3 pair.** Correction is proven on the
      WROOM/C3 pair (v0.2.0: -57.7 ppm uncorrected, zero underruns corrected, the
      two measures agreeing within 1.4 ppm). The S3 measured -30.5 ppm against the
      same source in v0.1.0, so it is a different offset on the same controller
      and worth confirming once it is plugged back in — it was disconnected
      partway through the session and never came back.
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

- [ ] **The C3 cannot source the bench stream.** Measured 2026-08-20: as the
      synthetic source it queued 37.8 pkt/s against the required 220.5 and
      starved the client into 159 underruns in 90 s. As a *client* it is fine —
      131,378 packets, zero underruns — which is the role it will actually have,
      so this only blocks a C3-only pair on the bench.

      The rate matches one packet per generation pass, so the generator is the
      bottleneck, not the radio (`qfull=0`, `senderr=0`). `benchServiceSource`
      calls `sinf` per sample and `2.0f * M_PI * BENCH_TONE_HZ * t` promotes to
      *double* because `M_PI` is a double — soft-float double on a chip with no
      FPU.

      The fix is a lookup table rather than a faster `sinf`: 2,205 samples is
      exactly 44 periods of 440 Hz at 22.05 kHz, so it wraps seamlessly, costs
      4.4 KB, and takes float out of the bench TX path on every board.
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
