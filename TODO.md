# SonoLoco TODO

## Current State (2026-08-18)

Full ESP-NOW mesh **is implemented** in `main.cpp` (this file previously said it was
commented out — that was stale). The firmware runs the three-state machine:
DISCOVERY / SERVER / CLIENT, with BT A2DP in, 22.05 kHz mono over ESP-NOW out,
and a jitter-buffered I2S client path.

All three PlatformIO environments compile clean. Nothing on the mesh path has been
validated on hardware yet.

---

## Done — audit fixes (not yet flashed or tested)

- [x] `i2s_write` partial writes handled (`i2sWriteAll`; client advances read pointer
      by bytes the DMA actually accepted, instead of discarding the rest)
- [x] Jitter buffer is block-atomic — overflow drops a whole packet instead of
      individual bytes, which used to desync 16-bit sample framing permanently
- [x] RX callback validates payload length against the received length, reads the
      header unaligned-safely, and forces an even byte count
- [x] Client locks onto the first sender MAC — two servers can no longer interleave
- [x] Lost packets are replaced with equivalent silence (capped at 4 packets)
- [x] `a2dpSink.end(false)` instead of `end(true)` — a node can go CLIENT → SERVER
      again without a power cycle
- [x] ESP-NOW TX gated on the send-complete callback; error counters instead of
      per-packet `Serial` logging in the callbacks
- [x] 4-tap `[1 3 3 1]/8` FIR before 2:1 decimation (an earlier off-tree bench put the
      20 kHz alias ~50 dB down vs 0 dB for naive decimation; the test file is not in
      this repo, so treat the number as unverified)
- [x] `esp_netif_init` / default event loop / `WIFI_PS_NONE` added to WiFi bring-up
- [x] WROVER PlatformIO environment added

---

## Done — second audit pass (2026-08-18, compiles, still not flashed)

- [x] **The firmware did not compile at all** for either BT environment: the global
      `btStarted` collided with Arduino's `bool btStarted()` in `esp32-hal-bt.h`.
      Renamed to `btSinkStarted`. All three environments now build clean.
- [x] Duplicate/reordered/restarted-server packets no longer read as a 65535-packet
      loss. `gap` is unsigned, so anything behind `lastSeq` used to charge ~65535 to
      `lost` and splice 800 bytes of bogus silence into the stream. Exact retransmits
      are now dropped (`dup`), large jumps re-baseline the sequence (`rsy`).
- [x] `resetRxState()` runs on *every* entry to DISCOVERY, not only when leaving
      CLIENT. A `senderLocked` left over from SERVER mode made a node ignore every
      future server permanently; a stale `rxActive` sent it into CLIENT mode on a
      stream that had stopped minutes earlier. A SERVER no longer locks a sender.
- [x] Failed CLIENT entry backs off `CLIENT_RETRY_BACKOFF_MS` instead of retrying
      every loop pass — that thrashed BT stop/start while a server was broadcasting.
- [x] `jAdvance()` rounds the I2S byte count down to whole stereo frames, so a partial
      write cannot shift the jitter buffer's 16-bit framing.
- [x] `static_assert`s on `ESPNOW_PAYLOAD_SIZE` (≤ 246, even).
- [x] Uncounted `jPushSilence` failures now increment `ovf`.
- [x] `wcfg.static_rx_buf_num` restored to the IDF default of 10. It had been cut to 4
      "because PSRAM handles the rest" — it does not: WiFi static RX buffers are
      DMA-capable internal DRAM and cannot live in PSRAM. A four-deep receiver against
      a continuous ~220 pkt/s stream was a likely source of unexplained `lost` counts.
      Costs ~10 KB of DRAM; watch the "Heap after WiFi init" line on the bench.
      `dynamic_tx_buf_num` stays at 4 — the send semaphore keeps one frame in flight.
- [x] `esp32-code/README.md` deleted; its audio-path diagram moved into the main
      README, everything else was a duplicate of it.
- [x] `platformio.ini` collapsed from three build configs to two. WROOM and WROVER now
      share one binary — the Arduino core has `CONFIG_SPIRAM=y` with
      `CONFIG_SPIRAM_BOOT_INIT` unset, so `psramInit()` probes at boot and only warns
      when there is no PSRAM. Verified: both envs compile with byte-identical flags.
      The three env *names* remain, as port aliases over the shared config.
- [x] Removed a trap in the S3 section: its comment told you to uncomment a second
      `build_flags` line, which would have been a duplicate key in one INI section —
      a hard `DuplicateOptionError` that stops the whole project from loading, not
      just that environment.
- [x] Tuneables moved out of `main.cpp` into `config.h` per the project convention
      (`TX_WARMUP_MS`, `CLIENT_BATCH`, `MAX_GAP_FILL_PKTS`, tone rate/amplitude/fade).
      `TONE_SAMPLE_RATE` is now `BT_SAMPLE_RATE`, which is what it always assumed.

---

## Next — hardware test bench

- [ ] Two ESP32 on this PC, both on COM ports, driven from the terminal
- [ ] Confirm which board is on which port and whether either has PSRAM
      (WROOM without PSRAM cannot run the mesh — `setupESPNow()` bails by design)
- [ ] Define per-step pass criteria readable from the serial log:
  - boot + heap + PSRAM report
  - DISCOVERY → SERVER on phone connect
  - server TX counters stay at zero (`qfull`, `senderr`, `radiofail`)
  - client reaches "Jitter buffer ready" and holds a stable `jitter=` fill
  - `lost` / `ovf` / `und` / `dup` / `rsy` counters over a 5-minute stream
- [ ] Scripted serial capture so runs can be compared between changes

---

## Still open — design issues

### Timing / sync
- [ ] **Server and clients are not time-aligned.** Server plays through A2DP with
      tens of ms latency; clients play after a ~185 ms jitter buffer. Adjacent
      rooms will slap-echo. Server needs to delay its own local playback.
- [ ] **No clock-drift correction.** Server BT clock and client I2S clock diverge;
      the jitter buffer will creep to overflow or underrun over minutes. Needs slow
      fill-level feedback (drop/duplicate a sample occasionally).
- [ ] **Sample rate is assumed to be 44.1 kHz.** If A2DP negotiates 48 kHz the
      clients play at the wrong pitch. Read the actual rate from the sink.

### Bandwidth
- [ ] 44 KB/s of ESP-NOW while BT Classic shares the same radio is thin.
      IMA ADPCM (4:1, cheap) would bring it to ~11 KB/s.

### Audio quality
- [ ] Server plays 44.1 kHz stereo locally, clients get 22.05 kHz mono —
      rooms will not sound alike. Decide whether that is acceptable.

### Housekeeping
- [ ] `String` concatenation in every `LOG_*` call fragments the heap — switch to
      `printf`-style
- [ ] Connect/disconnect tones write into `I2S_NUM_0` while the A2DP task also owns
      it; sequence them properly instead of interleaving
- [ ] Rename `esp32-code` to something consistent with the project name

---

## EMI / "frying" noise (hardware, unchanged)

- [ ] I2S wire lengths <10 cm, twist BCK/WS/DATA together
- [ ] 100nF + 10µF decoupling close to ESP32 VCC and PCM5102 VCC
- [ ] Single-point grounding across ESP32 / PCM5102 / TPA3116
- [ ] Ferrite beads on I2S lines if noise persists
- [ ] Separate 3.3V LDO for analog vs digital
