# Code Layout

Where each part of SonoLoco lives, and why there. The README says what the
project is and how to use it; this is for changing it.

Deliberately small. `main.cpp` is one file on purpose — resist splitting it
further; the exception below is argued in `docs/decisions.md` (D7).

- `esp32-code/src/main.cpp` — the state machine, tones, ESP-NOW TX/RX, A2DP
  callbacks and I2S setup, and `commandRun()`, the one dispatcher every
  command goes through, typed on the port or sent over the mesh (D16).
  Everything that needs real hardware.
- `esp32-code/include/config.h` — every tuneable number. New constants go here,
  never inline in `main.cpp`. Per-node values go in `platformio.ini` instead.
- `esp32-code/lib/jitter/` — the client's ring buffer (`jitter.h`), packet
  sequence accounting (`seqtracker.h`), where each lost block's silence
  went so a later packet can patch it (`holes.h`), the blocks held for undoing
  a parity (`blocks.h`), which packets were missed, one bit each, for
  scoring redundancy offline (`losstrace.h`), what a lost block plays
  instead of silence (`conceal.h`), and a node's own volume trim
  (`gain.h`). Pure logic, no Arduino or
  ESP-IDF, so it can be tested on a PC. This is where both of the worst bugs in this project
  lived.
- `esp32-code/lib/mesh/` — the mesh name to mesh id derivation. Pure arithmetic
  on a string, and in a library because two nodes disagreeing about what a name
  hashes to produces silence with nothing in the log — the one failure mode
  worth pinning on the host rather than chasing on a bench. See D12. Also which
  node an update request names (D15): a match too loose reboots the wrong
  speaker off the mesh. And the commands that travel the mesh (D16): their
  packets, which node one is for, and which may never go to every node.
- `esp32-code/lib/adpcm/` — the mesh codec: IMA ADPCM, stereo, in blocks that
  each carry their decoder state, so any block decodes alone. Pinned to the
  Python reference in `tools/codec/abtest.py` by golden vectors: two nodes built
  from codecs that disagree decode each other into noise. See D5.
- `esp32-code/lib/drift/` — the clock-drift controller. Decides when a client
  should duplicate or drop a sample to hold its buffer at depth; it never touches
  I2S or the ring buffer itself, which is what makes the closed loop simulable on
  a PC. See D11.
- `esp32-code/lib/sync/` — playing in time: a node's output clock, read from
  its own blocking DMA writes, and the server's schedule as a client sees it
  through the packets' `due` stamps, earliest of each window. The arithmetic
  behind D14, where an error is a node playing cleanly a few ms away from the
  others — which nothing but a microphone would notice.
- `esp32-code/test/test_jitter/` — host tests for the ring buffer and sequence
  accounting. Each one corresponds to a real bug or a real invariant.
- `esp32-code/test/test_adpcm/` — the codec against the reference's own output,
  byte for byte, including the block layout that is the wire format.
- `esp32-code/test/test_mesh/` — host tests for mesh identity, including known
  names pinned to known ids: changing the hash would split every deployed mesh
  silently, so it should take a failing test to do it.
- `esp32-code/test/test_drift/` — host tests for the controller, including
  hour-long closed-loop simulations at the drift measured on these boards. The
  model is checked against the recorded 1.34 B/s slope before anything built on
  it is believed.
- `esp32-code/test/test_sync/` — host tests for `lib/sync`: wake latency, an
  output slower than nominal, a window of slow packets, a server clock with
  its own slope, a schedule that moves, and the 32-bit wrap in every one.
- `esp32-code/test/unity_config.h`, `.cpp` — Unity's output for every suite,
  in place of the glue PlatformIO generates, which ends a run on
  `Serial.end()` and takes a C3 or S3 off USB (`docs/gotchas.md`).
- `esp32-code/scripts/version.py` — a PlatformIO pre-build step that defines
  `FW_VERSION` from `git describe`. There is no version constant to bump by hand;
  see D10.
- `tools/airmon/` — a standalone sniffer for a spare classic ESP32: what is on
  the channel, second by second, and a 13-channel survey. `capture.py` logs it,
  `correlate.py` lines it up with a bench log.
- `tools/bench-mesh.ps1` — the automated multi-board mesh test: discover, flash,
  stream, measure drift, report. Scales to any number of boards.
- `tools/test-client-only.ps1` — checks that a BT-capable board really works as a
  mesh client on a *normal* boot. `bench-mesh.ps1` cannot cover this, because it
  puts every node into bench mode by design.
- `tools/capture-serial.ps1` — timestamped serial capture of a single node, so
  two manual runs can be compared.
- `tools/ota.ps1` — updates a node over the home WiFi, relayed by any node on
  USB: build, request, find on the LAN, upload, read the version back. D15.
  `-Status` reads a node's WiFi signal and reception without uploading.
- `tools/ota-wifi.ps1` — stores the home WiFi on a node, once, over USB. The
  password is typed at a masked prompt and never passes through anything else.
- `tools/mesh.ps1` — runs any command on any node over the mesh, relayed by a
  node on USB; with no arguments, lists the mesh and draws who hears whom. D16.
- `tools/common.ps1` — what `ota.ps1` and `mesh.ps1` share: finding a relay,
  opening its port without resetting it, reading its lines.
- `tools/codec/abtest.py` — hear what a client plays before it is firmware: a
  WAV in, the original, the old 22.05 kHz mono path and the ADPCM path out, at
  the same rate and level. It decided D5, and its encoder is the reference
  `lib/adpcm` is tested against.
- `tools/btlisten/` — the Bluetooth half's test signal. The PC streams a 997 Hz
  tone to a server and records it with its own microphone, then counts holes,
  clicks and pitch error; with `--serial` the board's `a` window for the same
  seconds is printed beside it. `mon.py` logs that window every 2 s during
  ordinary use. `sync.py` measures how far apart the nodes play: clicks
  through the server, one node unmuted at a time. `soak.py` is the long
  silent run behind the server, and with `--trace` records which packets
  each client missed; `losstrace.py` then replays the firmware's rebuild
  rules on that record for every redundancy scheme a server can send, so
  schemes are compared on the same losses rather than in different minutes.

Comments in the code explain *why*, particularly where a line looks wrong but isn't.
