# Changelog

What has actually changed, newest first. This used to live in `TODO.md`, where a
growing list of completed work crowded out the open one.

Entries here are the *record*; the reasoning behind the standing design choices
is in `docs/decisions.md`, and the traps worth not re-introducing are in the
README's gotcha list.

---

## 2026-08-19 — Structure and docs

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

Bluetooth A2DP sink, I2S output to the PCM5102, notification sounds and basic
volume control. This part has run on hardware.
