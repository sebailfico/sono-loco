# Changelog

What has actually changed, newest first. This used to live in `TODO.md`, where a
growing list of completed work crowded out the open one.

Entries here are the *record*; the reasoning behind the standing design choices
is in `docs/decisions.md`, and the traps worth not re-introducing are in the
README's gotcha list.

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

---

## Unreleased

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
session, `v0.1.0-7-g778986f`:

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
