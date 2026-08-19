# SonoLoco TODO

What is **open**. Completed work is in `CHANGELOG.md`, standing design choices
and their reasoning are in `docs/decisions.md`. Keep those three separate — this
file previously carried all of it and the open list got lost inside the done one.

## Current state (2026-08-19)

The full ESP-NOW mesh is implemented in `main.cpp` and compiles for all three
environments. The ring buffer and packet sequence accounting are covered by host
tests. **Nothing on the mesh path has been run on two boards yet** — treat it as
unproven until `docs/bench-test.md` has been worked through.

---

## Blocking the test bench

- [ ] **Install a host compiler** so `pio test -e native` can run — there is no
      gcc/clang/MSVC on this machine, only the PlatformIO cross-toolchains.
      `winget install -e --id MSYS2.MSYS2` then `pacman -S
      mingw-w64-ucrt-x86_64-gcc`, and put `C:\msys64\ucrt64\bin` on PATH.
      Not strictly required: `pio test -e esp32dev` runs the same tests on a
      connected board.
- [ ] **Confirm the COM ports** with `pio device list` — COM7/COM8/COM9 in
      `platformio.ini` are guesses — and confirm which boards actually have PSRAM.
- [ ] **Run `docs/bench-test.md` end to end** on two boards, at least one WROVER.
      Record the captures; `tools/capture-serial.ps1` writes comparable logs.

---

## Open design issues

### Timing / sync

- [ ] **Server and clients are not time-aligned.** The server plays through A2DP
      with tens of ms latency; clients play after a ~185 ms jitter buffer.
      Adjacent rooms will slap-echo. The server needs to delay its own local
      playback to match.
- [ ] **No clock-drift correction.** The server's BT clock and the client's I2S
      clock diverge, so the jitter buffer will creep to overflow or underrun over
      minutes. Needs slow fill-level feedback — occasionally drop or duplicate a
      sample. Step 3 of the bench test measures the drift direction.
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
