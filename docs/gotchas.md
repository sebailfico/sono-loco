# Gotchas That Have Already Bitten This Code

Each of these was a real bug, found on hardware. Don't re-introduce them —
read the section for the part of the code you are changing first. Why the
design is what it is lives in `docs/decisions.md`; these are the traps in
carrying it out.

## The audio path

- **`i2s_write()` may accept fewer bytes than requested.** Always use `i2sWriteAll()`,
  or advance by the returned `bytes_written`. Ignoring it silently discards audio.
- **The jitter buffer must be pushed whole blocks or not at all, and moved in
  whole stereo frames.** Dropping an odd number of bytes shifts every later 16-bit
  sample by one byte and never re-aligns — a permanent noise stream, not a glitch.
  Moving by two bytes instead of four swaps left and right for the rest of the
  stream. Every push, advance and drift correction is a multiple of
  `CLIENT_FRAME_BYTES`. `JITTER_BUF_SIZE` must stay a power of two (a
  `static_assert` enforces it).
- **Mute by zeroing samples, not by switching an output off.** `m` once
  switched the A2DP library's output off, the library then kept its I2S
  driver installed, and the node's next CLIENT install failed every 5 s:
  stuck in DISCOVERY, counting packets and playing none. `m` zeroes what the
  ring hands to I2S, on every node, and the library's output is off for good
  (D14).
- **Every write to the output driver must be counted in `outFrames`, tones
  included.** The output clock (D14) reads the DMA position as that count
  modulo the buffer length; a write that bypasses it puts every later
  reading off by up to a buffer (5.8 ms), in a way no counter shows.
  `outWrite()` and `i2sWriteAll()` both count.
- **The ring's frame numbering must advance for every frame that enters the
  ring, silence included, and for nothing else.** `ringPushed` is what a
  packet's `due` and a client's timeline are expressed in. A push that fails,
  or a gap longer than the silence it was given, leaves the numbering behind
  the stream, and the timeline is flushed (`timelineFlush`) rather than
  steered by.
- **`JITTER_PREFILL` must exceed what the I2S DMA ring can swallow in one pass.**
  At 2000 bytes against a 4096-byte DMA ring, every arming of the jitter buffer
  was drained instantly and the client underran 24 times a second forever, while
  the audio limped along on DMA buffering alone. `static_assert`s tie the two
  constants together now.

## ESP-NOW and the mesh

- **Never `Serial.print` from the ESP-NOW send/recv callbacks.** They fire ~390×/s and a
  blocking UART write there causes the very dropouts it would be reporting. Bump a
  counter, print from `loop()`.
- **`esp_now_send()` back-to-back** without waiting for the send callback returns
  `ESP_ERR_ESPNOW_NO_MEM` and drops silently. TX is gated on a semaphore.
- **A node on an older firmware must not hear the new stream.** Before ADPCM a
  client pushed any payload straight into its buffer as PCM; fed ADPCM, that is
  full-scale noise through the amp. The mesh id on the wire is XORed with
  `MESH_WIRE_FORMAT`, so a node on the other format drops these packets as a
  foreign mesh (`fgn=` climbing). Change it whenever the payload changes
  meaning. (The old path's own trap — decimation that skips the FIR folds
  11–22 kHz into the audible band — went with the decimation.)
- **Sequence-gap arithmetic is unsigned.** A duplicate or reordered packet computes as
  a gap of ~65535; it has to be treated as a resync, not as 65535 lost packets.
- **Role state must be cleared on every entry to DISCOVERY,** not just when leaving
  CLIENT. A stale `senderLocked` makes a node ignore every future server forever.
- **The mesh id must be checked before the sender lock, not after.** A client
  locks onto the first node it hears; if a neighbour's packet reaches that lock
  before the id comparison, the node pins itself to a mesh it will then ignore
  every packet from, and stays deaf to its own household until it next falls
  back to DISCOVERY. Same shape as the stale-`senderLocked` bug above.
- **A pairing beacon must be filtered out before the sequence tracker.** A
  beacon is an audio packet with no payload and `MESH_BEACON_SEQ` in the
  sequence field; let one reach `SeqTracker` and it computes a gap of tens of
  thousands, charges a resync and re-arms the jitter buffer — an audible
  interruption caused by a node that was only saying hello.
- **ESP-NOW broadcasts at 1 Mbps unless told otherwise, and at 220 packets/s
  that is half the channel.** The default rate for ESP-NOW frames is 1 Mbps
  DSSS with a long preamble; a 206-byte frame is ~2.2 ms of air. With
  anything else on channel 1 the weakest receiver loses one packet in ten and
  the strongest loses none, in bursts every board sees at the same moment,
  which looks exactly like "clients degrade each other" until you check the
  timestamps. `ESPNOW_PHY_RATE` sets 12 Mbps; measured, see D13.
- **`len` is three fields: mask it with `ESPNOW_LEN_BYTES` before using it as
  a length.** The low byte is the payload length, bits 8–14 the redundancy
  distance, bit 15 the repeat flag. The first build that set the repeat bit
  also sent it: `ESPNOW_HEADER_SIZE + pkt.len` asked the radio for a 33 KB
  frame. The receiver splits it as it reads the header, before anything else
  looks.

## Bluetooth

- **`a2dpSink.end(true)` frees the BT controller permanently.** The node can then never
  be a SERVER again until it is power-cycled. Use `end(false)`.
- **…but `end(false)` alone leaves Bluetooth running.** It deinitialises A2DP and
  AVRCP only: Bluedroid and the controller stay up, still page- and
  inquiry-scanning like a speaker waiting for a phone, and coexistence hands
  those scans the radio. A WROVER client lost 4.7% of the mesh, in runs of 8+,
  next to an S3 losing 2.0%. `stopBluetooth()` disables Bluedroid and the
  controller after it, keeping the controller's memory.
- **Disable Bluedroid, never deinit it, behind the A2DP library's back.** The
  library remembers having initialised it and skips `esp_bluedroid_init()` on
  the next `start()`, which then loops forever on "Failed to enable bluedroid"
  — the node is wedged and never becomes a speaker again.
- **A node streaming Bluetooth loses mesh frames to its own radio, and the
  send callback calls every one a success.** 12–24% of broadcasts, one frame at
  a time, with `senderr=0 radiofail=0` on the server and nothing else on the
  channel. Only a receiver's `lost` shows it, and every client loses the same
  packets. That is why every block goes out twice, the second time 11 packets
  later (D13), past the 4–5-packet runs the server drops — and why "the server
  reports it sent them" proves nothing.
- **A node that runs Bluetooth cannot turn WiFi power save off.** The IDF
  coexistence layer requires modem sleep while the BT controller is enabled and
  enforces it with `abort()`: `esp_wifi_set_ps(WIFI_PS_NONE)` before BT starts
  dies in `coex_core_enable`, after BT starts it dies in `pm_set_sleep_type`
  from the WiFi task. Either way a boot loop with no message but a backtrace.
  Found on the first boot of the first WROVER — the WROOM never reached this
  code because it bails before WiFi, and the S3/C3 have no BT. `setupESPNow()`
  now only sets `WIFI_PS_NONE` on a boot that will never start Bluetooth. The
  modem sleep a BT node keeps instead costs it no packets (measured
  2026-10-01): it engages only while associated with an AP.

## Tasks and memory

- **Elapsed-time comparisons against a timestamp another task writes must be signed.**
  `millis() - lastRxMs` is unsigned, so a timestamp written one millisecond *after*
  this task read `millis()` wraps the difference to ~4.29 billion and every threshold
  test passes. That is not hypothetical: a client dropped to DISCOVERY announcing five
  seconds of ESP-NOW silence while its own receive counter was advancing by 221 packets
  a second (`silence now=13822 last=13823 age=4294967295`). Cast to `long` —
  `(long)(millis() - then) > TIMEOUT` — as `clientRetryAfterMs` already does.
  Comparisons against `loop()`'s own bookkeeping are safe, because only one task
  writes them.
- **Bench mode's flag must be `RTC_NOINIT_ATTR`, not `RTC_DATA_ATTR`.**
  `.rtc.data` is re-initialised from the image on every boot that runs the
  bootloader, so the flag reads back as zero and the node reboots into normal
  mode instead.

## Boards, boot and USB

- **Pairing is a long press while running, never a press held through a reset.**
  BOOT is a strapping pin: held low across a reset it puts the chip into the ROM
  download mode, where no firmware runs at all and nothing can react to the
  button. (It is GPIO 9 on a C3 devkit and GPIO 0 on the others.)
- **On a C3, GPIO 9 is not a ground.** It is BOOT. A DAC grounded on it held
  the board in download mode through every reset and replug: flashing worked,
  then nothing ran and the port stayed silent. Ground the DAC on a G pin. The
  strap is sampled at a power-on or watchdog reset, not at an RTS one: once
  the pin was free, RTS resets still came back in download mode until
  esptool's `--after watchdog_reset` (a replug does as well) sampled it again
  (2026-10-02).
- **The ESP32-S3 needs `-DARDUINO_USB_CDC_ON_BOOT=1`.** Its board definition sets
  `ARDUINO_USB_MODE=1` but leaves CDC off, so `Serial` goes to GPIO43/44 while
  the board enumerates on native USB — completely silent over the cable you are
  plugged into.
- **…and then `Serial` can stall the audio.** Native USB serial waits up to
  100 ms per write for a host that is plugged in but not reading, and `loop()`
  feeds I2S: an S3 on a PC with its port closed overflowed 9 times a minute.
  `setup()` gives it a 4 KB buffer and a 5 ms timeout — never 0, which the core
  turns into forever.
- **No `sinf()` and no `double` per sample: the C3 has no FPU.** On the
  classic ESP32 and the S3 single-precision float is hardware; on the C3 every
  float operation is a library call, and `double` is that on all of them —
  `M_PI` is a double, and one in an expression promotes the rest. The bench
  tone ran at a sixth of real time on a C3 until it became a table, and the
  startup beeps stuttered — 1,410 ms for 330 ms of sound, the DMA playing the
  gaps as silence — until they became a recurrence (2026-10-03). Anything a
  C3 computes per sample has to be measured on a C3.
- **`pio test` must not end on `Serial.end()` on a C3 or an S3.** PlatformIO's
  generated Unity glue does, and on these chips `Serial` is the chip's own
  USB port: a C3 dropped off USB the moment its tests had passed, and again
  on every power-up after — the test image running again — until it was held
  in download mode with BOOT and reflashed. `test/unity_config.h` and `.cpp`
  replace that glue, and their presence in `test/` is what stops PlatformIO
  generating it (2026-10-02). A classic board's CH340 never showed it.

## Build configuration

- **Don't name a global `btStarted`.** Arduino's `esp32-hal-bt.h` already declares
  `bool btStarted()` at global scope and the collision is a hard compile error.
- **Never put two `build_flags` keys in one `platformio.ini` section.** Duplicate keys
  in a single INI section are a hard `DuplicateOptionError` — the whole project stops
  loading, not just that environment. Extend a base section instead.
- **A `build_flags` in an `[env:...]` section replaces the parent's, it does not
  add to it.** Writing `build_flags = -DROOM_NAME='"Kitchen"'` under
  `extends = esp32_classic` silently drops `-DENABLE_BLUETOOTH` and the node
  quietly builds as a client. Always start the list with
  `${esp32_classic.build_flags}`. This one fails silently, unlike the duplicate
  key above — which makes it worse.
