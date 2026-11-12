#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// Room / Device Identification
// ============================================================================
// Visible as the Bluetooth speaker name, and the name `@` and the setup page
// know the node by.
//
// The node keeps its own in NVS once it is given one (`I<name>`, or the setup
// page, D19); this is only the name it has until then. Set it per node in
// platformio.ini (-DROOM_NAME='"Kitchen"'), not here, so the source tree stays
// identical for every board and you flash an environment rather than an edited
// file. This fallback only applies to a build that does not define it.
#ifndef ROOM_NAME
#define ROOM_NAME "SonoLoco"
#endif

// ============================================================================
// Firmware Version
// ============================================================================
// Injected at build time by scripts/version.py from `git describe`, e.g.
// "v0.1.0-3-gabc1234" (or with a trailing "*" for a dirty tree). There is no
// version constant to maintain here on purpose -- see D10 in docs/decisions.md.
//
// This fallback only applies to a build with no git available; if a board
// reports "unknown", nothing it measures can be tied to a commit.
#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif

// ============================================================================
// I2S / PCM5102 DAC Pin Configuration
// ============================================================================
// PCM5102 wiring, classic ESP32 (WROOM / WROVER):
//   PCM5102 BCK  -> GPIO 26
//   PCM5102 DIN  -> GPIO 25
//   PCM5102 LCK  -> GPIO 22
//   PCM5102 SCK  -> GND        (PCM5102 generates clock internally)
//   PCM5102 FMT  -> GND        (standard I2S format)
//   PCM5102 XSMT -> 3.3V       (soft mute off)
//   PCM5102 FLT  -> GND        (normal latency)
//   PCM5102 DEMP -> GND        (de-emphasis off)
//
// The pin numbers are per target because they have to be: GPIO 22-25 do not
// exist on an ESP32-S3 at all, and GPIO 26 is a flash/PSRAM pin there. The
// classic numbers were being handed to every board regardless, which is a
// silently invalid pin map on anything but a WROOM or WROVER.
//
// Override per node in platformio.ini (-DI2S_BCK_PIN=…) if a board is wired
// differently; these are only defaults.

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
// Free on both an S3 devkit and a C3 devkitm-1, clear of the strapping pins
// (0/2/3/8/9/45/46), the native USB pair (18/19 on a C3, 19/20 on an S3),
// UART0, and the flash and octal-PSRAM banks (26-37 on an S3).
// Confirmed 2026-08-26: wired and working on a C3 -- bench mode's tone was
// audible on its DAC -- and 2026-09-30 on the S3, into a MAX98357A.
#define I2S_BCK_PIN  4
#define I2S_WS_PIN   5
#define I2S_DATA_PIN 6
#else
// Classic ESP32. Wired and working on hardware.
#define I2S_BCK_PIN  26
#define I2S_DATA_PIN 25
#define I2S_WS_PIN   22
#endif

// ============================================================================
// Bluetooth A2DP (SERVER mode — BT Classic nodes only)
// ============================================================================
#define BT_SAMPLE_RATE   44100
// The A2DP volume a server starts at, 0-127. A phone or PC adopts it as its
// slider when it connects (AVRCP absolute volume), and keeps whatever it sets
// afterwards until the node reboots or passes through CLIENT. 38 is 30%. It
// was 1 while WROVER2's TPA3116 played every stream far too loud; each node's
// own trim (`v<dB>`, NVS) has tamed that since 2026-10-01, and starting at 1%
// only meant every connection began inaudible.
#define VOLUME_DEFAULT   38

// The server's own speaker plays from its own ring, exactly as a client's does
// (CLIENT_DMA_* and JITTER_PREFILL below), not through the A2DP library's I2S
// output -- see "Playing in time" and D14. Until 2026-09-30 it went through
// the library's driver with an 8 x 256 ring, prefilled at every stream start.

// ============================================================================
// The link — a two-chip server (D18)
// ============================================================================
//
// A front end (a classic board in front-end mode, `F`) runs Bluetooth and
// hands the phone's audio to a node over one UART: front end TX to node RX,
// node TX to front end RX, and ground. Every node listens on these pins, so
// the wiring is all a front end needs. Override per node in platformio.ini
// (-DLINK_RX_PIN=…) if a board is wired differently.
#if defined(CONFIG_IDF_TARGET_ESP32C3)
// UART0's own pins, free because the console is on native USB. The ROM prints
// its boot banner on 21 at reset; the far end's decoder skips it as noise.
#define LINK_RX_PIN  20
#define LINK_TX_PIN  21
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// Clear of the I2S pins, the strapping pins, native USB (19/20) and the
// octal-PSRAM bank (33-37).
#define LINK_RX_PIN  16
#define LINK_TX_PIN  17
#else
// Classic. Not 16/17, which are the PSRAM's on a WROVER.
#define LINK_RX_PIN  32
#define LINK_TX_PIN  33
#endif
#define LINK_UART_NUM  UART_NUM_1

// 4 Mbaud: 400 KB/s against the 176 KB/s of 44.1 kHz stereo PCM, and an exact
// divisor of the 80 MHz clock every target here runs its UART from. Short
// wires only -- a few centimetres between two boards in one box.
#define LINK_BAUD  4000000

// The UART driver's ring buffers. The receive side holds more than one A2DP
// packet from Windows (4 KB) while the receive task is between reads; the
// transmit side two, so the front end's A2DP callback never waits for the wire
// -- a chunk that does not fit is dropped and counted instead (`drops=`).
#define LINK_RX_BUFFER  8192
#define LINK_TX_BUFFER  8192

// Stereo frames per audio frame on the link: 1 KB, LINK_MAX_PAYLOAD in
// lib/link. An A2DP packet goes as several, back to back.
#define LINK_AUDIO_CHUNK  256

// The front end sends its whole state on every change and this often anyway,
// so a state lost on the wire is corrected within this. A node that hears no
// state for LINK_LOST_MS takes the front end, and its phone, as gone.
#define LINK_STATE_INTERVAL_MS  1000
#define LINK_LOST_MS            3000

// How long `>` waits for the front end's answer. The node's speaker keeps
// playing meanwhile; a front end on the far side of 4 Mbaud answers in a few ms.
#define LINK_REPLY_WAIT_MS  300

// The receive task: above loop(), which feeds I2S and would otherwise make the
// UART's buffer wait behind a blocking write, and below the WiFi and Bluetooth
// stacks. On core 0 where there are two, as the Bluetooth callbacks it stands in
// for run there.
#define LINK_TASK_PRIO   6
#define LINK_TASK_STACK  4096

// The front end's test signal (`s` in front-end mode): the bench tone, sent
// over the link the way Windows sends A2DP -- 1024 frames every 23.2 ms -- so
// a node's server path can be run with no phone.
#define LINK_TONE_PACKET_FRAMES  1024

// ============================================================================
// Notification Tones (startup / connect / disconnect)
// ============================================================================
// The tones use BT_SAMPLE_RATE directly. Connect/disconnect tones are written
// into an I2S driver that A2DP configured, so the tone generator cannot pick its
// own rate — there is deliberately no separate TONE_SAMPLE_RATE to drift from it.
//
// TONE_AMPLITUDE is the peak of a 16-bit tone sample, and it depends on the amp
// behind the DAC, so a node may set its own in platformio.ini. 500 (-36 dBFS)
// was chosen for a TPA3116, whose gain is high; into a MAX98357A at its 9 dB
// default the same tone is under a milliwatt, and those nodes set 4000.
#ifndef TONE_AMPLITUDE
#define TONE_AMPLITUDE   500
#endif

// Each sound is a few water drops through an echo (lib/tones): a sine that
// glides up into its note with a glassy shimmer on the attack, then repeats,
// quieter, DROP_ECHO_MS later. Chosen by ear on 2026-10-06 in a browser
// preview that runs the same arithmetic, tools/tones/preview.html, and
// test_tones holds the firmware to that preview's samples: retune there, then
// copy its settings here and its figures into the test.
//
// The notes are MIDI numbers, 69 being A4 at 440 Hz. Listening is two drops
// on the middle one, joined rises through all three, nothing heard falls.
// Doubles, not floats: lib/tones works out its coefficients from these once a
// sound, in double, so that they round as the preview's did.
#define DROP_NOTE_LOW          82      // A#5
#define DROP_NOTE_MID          85      // C#6
#define DROP_NOTE_HIGH         90      // F#6
#define DROP_GLIDE_DEPTH_ST    3       // a drop starts this far below its note
#define DROP_GLIDE_MS          14      // and closes on it with this time constant
#define DROP_DECAY_MS          115     // x1.8 on the last of three, x1.4 on the second of two
#define DROP_GAP_MS            160     // between drops
#define DROP_GLASS             0.5     // FM index in radians; 0 is a pure sine
#define DROP_GLASS_RATIO       3.5     // modulator : carrier
#define DROP_GLASS_MS          20      // the shimmer's time constant
#define DROP_ECHO_GLASS_KEEP   0.5     // shimmer left on each repeat
#define DROP_ECHO_MS           210
#define DROP_ECHO_LEVEL        0.13    // each repeat, relative to the one before
#define DROP_ECHO_REPEATS      3       // at most: one under -54 dB is not played, so 2 here

// The range of a node's own volume trim (`v<dB>`, lib/jitter/gain.h). Above
// 0 a loud passage clips; the ceiling is there so a typo cannot ask for +40.
#define OUT_TRIM_MIN_DB  -40
#define OUT_TRIM_MAX_DB   12

// ============================================================================
// Mesh Identity — which nodes belong to whose household
// ============================================================================
// Every packet carries a 16-bit mesh id, derived from a human-typed mesh name,
// and a client ignores anything that is not its own. Without it two SonoLoco
// installations in radio range join each other's music: the radio is shared,
// the channel is fixed and the destination is the broadcast address, so a
// neighbour's server is indistinguishable from yours. See D12 and lib/mesh/.
//
// This is NOT ROOM_NAME's counterpart in platformio.ini (D8). ROOM_NAME is per
// node, so it belongs to a build environment; the mesh name is per *household*
// and has to be settable on a board somebody already owns, so the live value
// lives in NVS and is set over serial ('g') or by pairing ('p' / a long press).
// What is below is only the factory default, used until a node is told
// otherwise — which means two households that both accept the default are in
// the same mesh, exactly as they are today. Pairing is what separates them.
#ifndef MESH_NAME
#define MESH_NAME "sonoloco"
#endif

// The node's one button, read by how long it was held when it is let go
// (lib/button): a short press opens Bluetooth for BT_WINDOW_MS on a node that
// can be a server (D17), a few seconds pair (D12), a long hold opens the
// setup network (D19). The devkit BOOT button on every board here, since it is
// the only one that exists; set to -1 to disable the button entirely and do
// all of it over serial (`B`, `o`, `p`, `C`).
//
// It is a press *while running*, never "hold it down at boot": BOOT is a
// strapping pin, and holding it through a reset puts the chip into the ROM
// download mode instead of into anything this firmware could react to.
// Guarded, so -DMESH_PAIR_BUTTON_PIN=… in platformio.ini overrides it cleanly
// rather than colliding with the definition below.
#ifndef MESH_PAIR_BUTTON_PIN
#if defined(CONFIG_IDF_TARGET_ESP32C3)
// C3 devkitm-1 wires BOOT to GPIO 9; the classic devkits and the S3 use GPIO 0.
#define MESH_PAIR_BUTTON_PIN  9
#else
#define MESH_PAIR_BUTTON_PIN  0
#endif
#endif

// The ranges, by the time the button is let go. Each starts at its first
// number and ends before its second; between them, and past the last, a
// release does nothing -- margin for a hand counting seconds, and a way out.
// Pairing starts at 3 s so it cannot be confused with the reset-adjacent
// fumbling that BOOT normally sees. A drop says when a hold has reached the
// next range, in DISCOVERY; while the node plays, the music is all there is.
#define BUTTON_PRESS_MIN_MS    50      // shorter is the contact bouncing, not a finger
#define BUTTON_BT_MAX_MS       1000    // a short press: Bluetooth
#define BUTTON_PAIR_MIN_MS     3000    // 3 to 9 s: pairing
#define BUTTON_PAIR_MAX_MS     9000
#define BUTTON_CONFIG_MIN_MS   10000   // 10 to 15 s: the setup network
#define BUTTON_CONFIG_MAX_MS   15000

// How long a short press keeps Bluetooth on, so a phone can connect. A node
// that has played as a client keeps it off otherwise (D17). A phone that
// connects in time makes the node a server as always; if none does, Bluetooth
// goes off again and the node rejoins whatever the mesh is playing.
#define BT_WINDOW_MS          60000

// How long a press stays live, at both ends.
//
// Pairing takes two presses: one on a node of the mesh being joined, which then
// offers itself, and one on the node being moved, which listens. Both windows
// have to be open at the same moment, so this is the time to walk from one to
// the other -- and it is also why a neighbour cannot capture a node by playing
// music at it. They would have to be holding their button inside this window.
#define MESH_PAIR_WINDOW_MS   60000

// How often an offering node repeats its beacon, ms. It is 5/s against the
// stream's 220/s, so it costs nothing measurable even if a server offers while
// it is playing; the listening node adopts on the first one it hears.
#define MESH_BEACON_INTERVAL_MS  200

// ============================================================================
// Update mode — new firmware over the home WiFi
// ============================================================================
// A node in a room has no cable to the PC. Asked over the mesh (`U<name>` on
// any node that is on USB), it reboots into update mode: off the mesh, onto the
// home network with the credentials stored by `W`, and an HTTP endpoint that
// takes one image -- tools/ota.ps1 drives all of it. See D15.

// How long to wait for the home network before giving up and going back to
// the mesh. A wrong password or an out-of-range router ends here.
#define OTA_CONNECT_TIMEOUT_MS  30000

// How long update mode waits for an upload to begin. A node must never be
// left off the mesh because a PC went away: when this runs out it reboots
// into normal mode, and so does any reset in between.
#define OTA_WINDOW_MS           300000

// How long a freshly updated image must run before it is kept. Until then the
// bootloader holds the previous one, and any reset -- a crash, the watchdog, a
// power cut -- boots that instead. Also cut short by the next update request,
// which is the most direct evidence there is that the new image works.
#define OTA_CONFIRM_MS          60000

// The request goes out as broadcast, which nothing acknowledges, so it is sent
// this many times this far apart. The target acts on the first it hears; the
// rest arrive at a node that has already left the channel.
#define OTA_REQUEST_COPIES       25
#define OTA_REQUEST_INTERVAL_MS  200

// Commands over the mesh, `@<target> <command>` (D16). Repeated like the
// update request, but a command is for a node that is up and listening rather
// than one that may still be booting: fewer copies, closer together. The node
// named runs the first copy it hears and ignores the rest.
#define MESH_COMMAND_COPIES       5
#define MESH_COMMAND_INTERVAL_MS  100
// A command sent to `*` reaches every node in the same instant, and broadcast
// is never retried: each node answers after a random part of this, so the
// house does not answer all at once and drown itself out. Shorter than the
// copies take to arrive, so that every node has later copies left to answer
// again for -- the retry an answer otherwise lacks.
#define MESH_REPLY_SPREAD_MS      300
// How long the asking node prints replies for, from the moment it asks. Has
// to cover the copies, the spread, and the command itself running.
#define MESH_REPLY_WAIT_MS        2500
// Most a command's answer may be, in bytes: one to four packets. On the
// running node's stack while the command runs, never kept.
#define MESH_REPLY_CAPTURE        768
// Replies heard but not yet printed, on the asking node only: made the first
// time it asks, about 2 KB, so a node nobody types `@` into never pays for it.
#define MESH_REPLY_QUEUE_DEPTH    8
// A question to one node is over this long after its answer begins: the rest
// of an answer's parts follow within a few ms, so a page does not wait out
// MESH_REPLY_WAIT_MS for every slider it moves.
#define MESH_REPLY_SETTLE_MS      300
// A question to one node that got no answer is asked again, this many times,
// if asking twice means the same as asking once (meshCommandRetryable): an
// answer is the only sign a command arrived (D16), and the setup page shows
// what the answer says rather than what it sent.
#define MESH_ASK_RETRIES          2

// ============================================================================
// The setup network (D19)
// ============================================================================
//
// An access point on the mesh's own channel, so the node goes on hearing and
// asking the mesh while a phone is on it, with a page that lists the mesh and
// sets each node through the same commands as `@`. Opened by a 10-15 s hold
// or `C`; on its first power-on, a node with no password set and no Bluetooth
// of its own opens it by itself for CONFIG_FIRST_WINDOW_MS.
//
// Open (no password) until somebody sets one on the page; WPA2 from then on.
// The home WiFi's password is accepted only once the network has a password,
// so it never crosses the air in the clear.
#define CONFIG_WINDOW_MS        600000   // a window opened by hand: 10 minutes...
#define CONFIG_IDLE_MS          300000   // ...and at least this long after the last request
#define CONFIG_FIRST_WINDOW_MS  300000   // the first power-on: 5 minutes
#define CONFIG_MAX_CLIENTS      2
#define CONFIG_AP_PASS_MIN      8        // WPA2's own limits
#define CONFIG_AP_PASS_MAX      63
// What a page's command may print back: `@* N` on a mesh of a dozen nodes.
#define CONFIG_REPLY_MAX        4096
// How long the page's request waits for loop() to run its command, retries
// included: three asks of MESH_REPLY_WAIT_MS and some.
#define CONFIG_CMD_TIMEOUT_MS   9000

// ============================================================================
// ESP-NOW Mesh
// ============================================================================
// Fixed channel — must be the same on every node.
//
// 11, not 1. A survey with tools/airmon on 2026-09-14 found the whole 2.4 GHz
// band in this house empty except channel 1, where the house router sits at
// -54 dBm — and every packet the mesh had ever lost was lost to that router's
// traffic (bench-20260914-161403 against air-20260914-161319: loss tracks the
// seconds the monitor saw hundreds of undecodable HE frames). Channel 11 is
// 50 MHz away from it. If the router moves, this should too; a per-mesh
// channel is in TODO.md.
#define ESPNOW_CHANNEL       11

// PHY rate for ESP-NOW frames. The IDF default for broadcast is 1 Mbps DSSS
// with a long preamble, at which a 206-byte packet occupies about 2.2 ms of
// air -- times 220.5 packets/s is roughly half the channel, on the most
// crowded 2.4 GHz channel there is. A 600 s run with four clients on
// 2026-09-14 lost 10.7% on the C3 and 2.2% on the S3 in multi-second bursts
// that hit every board at the same moments, i.e. somebody else's traffic
// winning the collisions. 6 Mbps OFDM cuts the airtime about six-fold for a
// few dB of receiver sensitivity, which a house does not miss. Every chip on
// the bench (ESP32, S3, C3) decodes 802.11g. Applies to what this node
// *sends*, so it is set on every node because any node can be a source. See D13.
//
// 12, not 6, since 2026-09-29: a server that is also streaming Bluetooth
// loses frames to its own BT link, and loses fewer the shorter they are. PC
// streaming A2DP into WROVER1, WROVER2 listening, one copy of each frame:
// 6 Mbps lost 12%, 12 Mbps 2.0%, 24 Mbps 4.0%, 54 Mbps 1.6% -- every loss a
// single frame. 12 costs about 4 dB of sensitivity against 6 (ESP32
// datasheet: -89 against -93 dBm); 54 would cost 17.
#define ESPNOW_PHY_RATE      WIFI_PHY_RATE_12M

// How many times each frame is sent. A broadcast has no acknowledgement and so
// no retry: a frame lost in the air, or to the node's own Bluetooth radio, is
// a hole in the audio unless the data went out twice. It now does, inside the
// packet -- every packet carries an older block too (MESH_BLOCKS_PER_PACKET)
// -- so one copy is enough. Two back-to-back copies were the first fix
// (2026-09-29: 12 Mbps with two copies lost nothing in 15 s, against 12% at
// 6 Mbps with one) and `t2` still sends them, for comparison; copies carry
// ESPNOW_LEN_REPEAT in main.cpp and the receiver plays the first that arrives.
#define ESPNOW_TX_COPIES     1

// Minimum spacing between two audio packets leaving the TX task, in µs; 0
// sends each the moment the radio is free. A Bluetooth server gets its audio
// an A2DP packet at a time -- 1024 frames from Windows, every ~23 ms -- and
// each becomes nine mesh packets at once, which unpaced leave in one 5-9 ms
// burst. `P<us>` changes it until reboot. A backlog deeper than
// ESPNOW_TX_PACE_BACKLOG goes out unpaced: pacing may delay the stream, never
// overflow the queue.
#define ESPNOW_TX_PACE_US       0
#define ESPNOW_TX_PACE_BACKLOG  12

// ============================================================================
// Mesh audio format
// ============================================================================
// 44.1 kHz stereo, IMA ADPCM: each sample a 4-bit step from a prediction, so
// the full-rate stereo stream costs 44 KB/s -- what 22.05 kHz mono PCM cost
// before (D5). The server forwards what A2DP gives it, with no decimation and
// no fold to mono; a client plays what the server plays. The codec is
// lib/adpcm; tools/codec/abtest.py is its reference and the A/B it was chosen by.
//
// Every node must agree on all of these, like ESPNOW_CHANNEL, and a node on
// another format is kept out by MESH_WIRE_FORMAT in main.cpp.
#define CLIENT_SAMPLE_RATE   44100

// Stereo frames per ADPCM block: 2.6 ms of audio. Each block is 6 bytes of
// decoder state plus one byte per frame, so a block is 120 bytes.
#define MESH_BLOCK_FRAMES    114

// Blocks per packet: its own, and an older one, MESH_REDUNDANCY_DISTANCE
// packets back. A block lost in the air plays as silence until the packet
// carrying it again arrives, which writes it over the silence in the client's
// buffer before it plays. Two 120-byte blocks and the 6-byte header are a
// 246-byte frame, 387 of them a second: about the airtime of the two-copy
// stream it replaced.
#define MESH_BLOCKS_PER_PACKET  2

// How far back the second block is, in packets (2.6 ms each). It has to reach
// past whatever took the first: a Bluetooth server loses its frames in runs
// of 4-5 packets, 10-13 ms, when its own link has the radio (D13), and a
// block one packet back was lost in the same run. It also has to arrive
// before the silence plays -- 15 is 39 ms, well inside the ring a client
// keeps on the server's schedule (JITTER_PREFILL). As a plain copy, the
// price of any distance above 1 was that a lone lost packet is no longer
// always saved: its copy can land on another loss. Measured as a copy behind
// a streaming server, alternating 60 s runs (2026-09-30): holes 1.17% at 1,
// 0.57% at 6, 0.33% at 11 -- and 0.7% at 11 over the 600 s after. The
// parity below removes that price, and 15 was the best distance for it on
// every recording scored (2026-10-01, 2026-10-03). Sent in every packet, so
// a server's `D<n>` or `X<n>` changes it for the whole mesh at once.
#define MESH_REDUNDANCY_DISTANCE  15

// Parity instead of a plain older block: the second block is the XOR of the
// blocks 1 and MESH_REDUNDANCY_DISTANCE back, and a client that holds either
// rebuilds the other. The same bytes buy both distances -- a lone loss comes
// back from the next packet, a run from the one DISTANCE later -- where a
// plain block buys one. `X<n>` on a server turns it on with distance n,
// `D<n>` off. Two traced 600 s soaks behind the streaming server on `X15`
// (2026-10-03): the firmware's holes matched the model's to within 3 blocks
// a client, 0.30-0.54% of blocks where a copy at 11 would have left
// 0.58-0.82% of the same losses.
#define MESH_REDUNDANCY_PARITY  1

// What a client plays for a block that never arrived and was not rebuilt: 1
// fills it from its two neighbours, each played backwards away from the edge
// it shares, so the waveform has no step at either edge (lib/jitter/conceal.h);
// 0 plays zeroes, a 2.6 ms drop and a click. A client setting, not on the
// wire: `z1` / `z0` until reboot.
#define MESH_CONCEAL  0

// Blocks a sender keeps to draw the older one from: the largest distance plus
// one. A power of two.
#define MESH_TX_HISTORY  16

#define MESH_BLOCK_BYTES     (6 + MESH_BLOCK_FRAMES)
#define ESPNOW_PAYLOAD_SIZE  (MESH_BLOCKS_PER_PACKET * MESH_BLOCK_BYTES)

// FreeRTOS queue depth for the ESP-NOW TX task (packets buffered before dropping).
// FreeRTOS queue storage is internal DRAM, one packet (~250 bytes) a slot. 8
// was tried on 2026-09-14 and dropped 23 packets the first time a phone paused
// and resumed: the A2DP decoder hands over a burst when audio restarts. 16 was
// three A2DP packets' worth then; one A2DP packet (1024 frames) is now nine
// mesh packets, not five, so it is 24 -- 2.7 A2DP packets, 62 ms, 5.9 KB.
// qfull is on the SERVER status line and in `a` to watch.
#define ESPNOW_TX_QUEUE_DEPTH  24

// Hold ESP-NOW TX for this long after BT audio starts, so the A2DP pipeline has
// settled before the radio starts competing with it.
#define TX_WARMUP_MS  1000

// --- WiFi driver buffer counts (see setupESPNow) -----------------------------
// Receive side carries ~387 packets/s continuously. These buffers are DMA-capable
// *internal* DRAM — PSRAM cannot back them, so trimming them is the only way to
// claw back DRAM, and trimming them too far drops audio. 10 is the IDF default;
// each buffer costs roughly 1.6 KB, so this is ~16 KB of DRAM.
#define WIFI_STATIC_RX_BUFFERS   10

// Transmit side is gated on the send-complete semaphore, so at most one frame is
// ever in flight. This core builds the WiFi driver with *static* TX buffers
// (CONFIG_ESP32_WIFI_STATIC_TX_BUFFER=y, 8 of them, ~1.6 KB each, allocated at
// init in internal DRAM); the dynamic count it used to set here was a field the
// driver never reads. 2 is one in flight and one being filled. Found while
// chasing the WROVER's BT-connect crash, 2026-09-14.
#define WIFI_STATIC_TX_BUFFERS   2


// ============================================================================
// Jitter Buffer (CLIENT mode)
// ============================================================================
// Absorbs network timing variation before writing to I2S. Holds decoded PCM,
// 4 bytes a stereo frame: 32768 bytes at 44.1 kHz stereo is 185 ms, the same
// depth as the 8192 bytes of 22.05 kHz mono it replaces. Allocated at boot, in
// PSRAM where there is PSRAM -- a WROVER server needs its internal DRAM for
// Bluetooth -- and from the heap elsewhere.
// MUST be a power of two — the ring uses masking, not modulo (static_assert
// in main.cpp enforces this).
#define JITTER_BUF_SIZE    32768

// Bytes of PCM per stereo frame, in the jitter buffer and on I2S. Everything
// the client moves -- a batch, a correction, a lost block's silence -- is a
// whole number of these, or the channels swap for good.
#define CLIENT_FRAME_BYTES  4

// Client I2S DMA ring. dma_buf_len counts stereo frames: 8 x 256 is 46 ms at
// 44.1 kHz, as 4 x 256 was at 22.05.
#ifndef CLIENT_DMA_BUF_COUNT   // a build flag may override it, for an A/B between clients
#define CLIENT_DMA_BUF_COUNT  8
#endif
#define CLIENT_DMA_BUF_LEN    256

// Bytes the DMA ring can swallow when completely empty: 8 * 256 * 4 = 8192.
#define CLIENT_DMA_CAPACITY_BYTES (CLIENT_DMA_BUF_COUNT * CLIENT_DMA_BUF_LEN * CLIENT_FRAME_BYTES)

// Minimum bytes in the jitter buffer before I2S output starts (~136 ms).
//
// This MUST exceed CLIENT_DMA_CAPACITY_BYTES, and the first hardware run is why:
// at 2000 bytes it was below the DMA capacity of the then 8-buffer ring, so
// every time the buffer armed, the DMA swallowed the whole prefill in one pass
// and the buffer immediately ran dry -- 24 underruns per second, forever, and a
// jitter buffer that never held more than a fraction of its intended depth. The
// audio survived on DMA buffering alone. A static_assert in main.cpp enforces
// the relationship now.
//
// Since D14 it is also the whole mesh's latency: the server starts its own ring
// from exactly this, and every client plays on the server's schedule, so the
// depth a client has is whatever the server's leaves it. 16000 (91 ms) was
// chosen for clients that each set their own. On the server's schedule it left
// them 15-48 ms of ring, often under the 28 ms a lost block's late copy needs
// (MESH_REDUNDANCY_DISTANCE): 600 s behind the server, 3,600 copies arrived
// too late to patch and 2.2% of blocks were holes, against 0.7% before
// (2026-09-30). A block's first frames can also reach a client a whole A2DP
// packet (23 ms) after the server had them, when the block straddles two
// packets. 24000 gives the copy and the straddle room: 136 ms, every room
// later together, so the only cost is lip-sync against the phone.
#define JITTER_PREFILL     24000

// Stereo frames handed to I2S per loop() pass.
#define CLIENT_BATCH       128

// ============================================================================
// Playing in time (every node that plays the stream) -- lib/sync, D14
// ============================================================================
// Measured with a microphone on 2026-09-30, before any of this: the S3 played
// 51 ms after the Bluetooth server and WROVER2 44 ms after it -- an echo
// between any two rooms. Now every packet says when the server plays its
// block (`due`), and each client plays it then.

// `due` is 16 bits in these units: 262 ms of range, far past any latency here.
#define MESH_DUE_UNIT_US      4

// The least time from a server's esp_now_send() to a client's receive
// callback: a 248-byte frame at 12 Mbps is ~0.2 ms of air, plus the channel
// access and the receive path. A client's estimate of the server's play time
// assumes exactly this much transit; every slower packet only looks later,
// and the earliest-of-window estimate ignores it. An error here is a constant
// offset between server and clients -- the microphone is what would show it.
#define MESH_TRANSIT_MIN_US   300

// The server's timeline is the earliest point of each window of stamped
// packets; this long a window (about 97 packets), at least this many points
// in it, and estimates no older than this.
#define SYNC_WINDOW_MS        250
#define SYNC_MIN_SAMPLES      16
#define SYNC_MAX_AGE_MS       1000

// Packets this far apart are a stream that paused: the server has re-armed
// its own output meanwhile, and the timeline starts again from nothing.
#define SYNC_STREAM_GAP_MS    150

// A client with the prefill in hand waits at most this long for a timeline
// before playing without one, as clients did before stamps existed.
#define SYNC_WAIT_MS          1500

// Reads of the output clock before a client aligns to the server: the first
// waits after an idle DMA can land on a ring still holding stale buffers.
#define SYNC_PRIME_OBSERVATIONS  4

// A write that returned after this long waited for a DMA buffer, and times the
// output clock (lib/sync). Copying a batch takes tens of microseconds; a wait
// ends in the buffer-done interrupt, anywhere up to a buffer (5.8 ms) later.
#define OUT_WAIT_MIN_US       300

// Beyond this error from the server's schedule, a client jumps back onto it --
// skipping what is late, or waiting out what is early -- instead of steering.
// Steering moves 227 us a second at DRIFT_MAX_RATE; this is what a server
// re-arming or a DMA running dry costs, not drift.
#define SYNC_JUMP_US          4000

// Steering onto the schedule: the drift controller, on the timing error.
// Proportional control parks at deadband + rate / kp: at the -44 ppm measured
// between WROVER2 and the S3 (1.9 corrections/s) that is 200 us + 38 bytes,
// about 0.4 ms. Loop time constant 1 / (kp * 4 bytes a correction) = 5 s,
// against a 1 s filter on an error that has no packet jitter left in it.
#define SYNC_DEADBAND_US      200
#define SYNC_KP               0.05f
// Twice DRIFT_MAX_RATE: a client has to follow every correction the server's
// own level controller makes (each moves the whole schedule) plus its own
// crystal against the server's, so its authority must exceed the server's.
// 20/s is 454 ppm; in steady state it uses a few per second.
#define SYNC_MAX_RATE         20.0f
#define SYNC_EMA_TAU_MS       1000.0f
#define SYNC_SETTLE_MS        1000

// ============================================================================
// Clock Drift Correction (CLIENT mode)
// ============================================================================
// Source and client run off separate crystals with nothing synchronising them.
// Measured over 600 s against one WROOM source: an S3 client drifts -30.5 ppm
// (1.34 B/s, buffer empty in ~18 min), a C3 client -57.7 ppm (2.55 B/s, ~10
// min). Two clients of one source, differing by nearly a factor of two, which is
// the case for a controller rather than a constant made by the hardware. See
// lib/drift/drift.h for the control law and test/test_drift for the closed-loop
// simulations these values were chosen against.
//
// The correction is one duplicated or dropped stereo frame at a time -- at
// 44.1 kHz those offsets are 1.35 and 2.55 frames/s, an edit every half second
// or so, which is why no resampler is needed.
//
// Every number below is in bytes of the jitter buffer, and the format behind
// them changed on 2026-09-29 from 22.05 kHz mono (2 bytes, 44,100 B/s) to
// 44.1 kHz stereo (4 bytes, 176,400 B/s). They were scaled to keep the same
// behaviour in *time*: bytes x4, corrections per second x2 for the same ppm.
// Byte figures quoted from measurements before that date are in the old unit.

// Bounds on the fill the controller steers towards. The target itself is
// measured once the settle window closes -- see lib/drift/drift.h -- and clamped
// into this band.
//
// Ring occupancy plus DMA content is conserved at JITTER_PREFILL, so the ring
// sits somewhere between the prefill less the DMA ring's capacity and the
// prefill itself. Measured on hardware the DMA holds about 1,880 of its 2,048
// bytes, putting the natural level near 2,250 rather than the 1,952 the
// arithmetic predicts. That 350-byte difference is not academic: it is depth the
// client does not have to spare.
//
// The ceiling is where the DMA would be under half full. After a settle window
// that means something is wrong, and a measurement that far out is not one to
// steer by.
#define DRIFT_TARGET_BYTES      (JITTER_PREFILL - CLIENT_DMA_CAPACITY_BYTES)
#define DRIFT_TARGET_CEIL_BYTES (JITTER_PREFILL - CLIENT_DMA_CAPACITY_BYTES / 2)

// The Bluetooth server's own level controller (D14) has a floor one A2DP
// packet lower: its ring is fed a whole packet at a time -- 4 KB from Windows
// every 23 ms, with gaps to 50 ms -- so its level runs up to a packet below a
// client's. With the client's floor, 600 s behind the PC on 2026-09-30, the
// server's measured level came out ~2 KB under it, the clamp raised its
// target, and it inserted at DRIFT_MAX_RATE for half a minute -- 300 inserts,
// each moving every client's schedule 23 us later, faster than they could
// follow: -2 ms and a jump on each.
#define SERVER_TARGET_BYTES     (DRIFT_TARGET_BYTES - 4096)

// How long after playback arms before corrections may start, ms.
//
// Arming happens at JITTER_PREFILL with an empty DMA ring, and the ring then
// takes its share within about 50 ms -- an 8,192-byte step down that is not
// drift and must not be corrected as if it were. Three filter time constants is
// enough for the smoothed fill to forget it. Drift takes minutes to matter, so
// the dead time costs nothing.
#define DRIFT_SETTLE_MS       12000

// No correction at all while the smoothed error is inside this band.
//
// 4.5 ms, which was one packet. Wide enough that packet-arrival jitter never
// provokes an edit -- and the 4 s filter below has already removed most of that
// anyway -- while costing only 4.5 ms of depth before the controller engages,
// which at the measured offsets is 150 s (S3) or 78 s (C3) of untouched drift
// at startup. It was twice that, and that is depth this buffer cannot spare:
// see the equilibrium arithmetic under DRIFT_KP.
#define DRIFT_DEADBAND_BYTES  800

// Corrections per second per byte of error outside the deadband.
//
// This sets where the buffer parks. Proportional control holds the level exactly
// far enough from target to generate the correction rate the drift demands, so
// the steady-state depth is
//
//     target - (deadband + required_rate / kp)
//
// and that depth has to survive a radio hiccup. It is not a free parameter: at
// 0.005 (old format) and the -58 ppm measured between the WROOM and the C3, the
// client parked at 29 ms. A client at that depth underran in the 600 s baseline
// on a two-packet loss, with 30 ms in the buffer one second earlier. At 0.02
// (old format) the same client parked near 45 ms. In today's format that is
// 0.01: the offset is required_rate / kp, and the rate doubles while the bytes
// quadruple. The loop time constant is 1/(kp * CLIENT_FRAME_BYTES) = 25 s: six
// times slower than the input filter, so there is nothing for it to ring
// against, and far faster than the drift it corrects.
#define DRIFT_KP              0.01f

// Hard cap on the correction rate, corrections per second. 10/s is 227 ppm of
// authority at 44.1 kHz, comfortably past any crystal pair, and bounds how much
// audio the controller can touch if something else goes wrong.
#define DRIFT_MAX_RATE        10.0f

// Time constant of the fill low-pass, ms.
#define DRIFT_EMA_TAU_MS      4000.0f

// Lost packets are replaced with an equal amount of silence to keep playback
// timing. Capped so one long outage can't flood the buffer with silence.
//
// 24 blocks, 62 ms, since D14. A gap longer than its silence leaves the ring
// shorter than the stream, and a client on the server's schedule then has to
// jump back onto it -- at 8 blocks (21 ms), 600 s behind a streaming server
// had 60 runs of 8+ lost blocks and 19-22 jumps per client (2026-09-30). 24
// covers the server's Bluetooth holding the radio for a burst, and on top of
// the ring's ~90 ms at depth still fits the 185 ms ring.
#define MAX_GAP_FILL_PKTS  24

// A sequence number this far from the expected one is treated as a stream
// restart, not as a gap. Sequence numbers are uint16_t, so a duplicate or
// reordered frame computes as a "gap" of ~65535 — filling that as loss would
// charge tens of thousands to the lost counter and inject bogus silence.
#define SEQ_RESYNC_THRESHOLD  64

// The loss trace (`L1`, lib/jitter/losstrace.h) is printed this often, at most
// LOSS_TRACE_LINE_PKTS packets to a line. A second at 387 pkt/s is one line of
// ~120 bytes; the trace holds ~10 s, so loop() can stall for nine before
// anything is dropped.
#define LOSS_TRACE_PRINT_MS    1000
#define LOSS_TRACE_LINE_PKTS   512

// If entering CLIENT mode fails (I2S unavailable), wait this long before trying
// again. Without it, an incoming stream retriggers the attempt every loop pass
// and thrashes BT stop/start.
#define CLIENT_RETRY_BACKOFF_MS  5000

// ============================================================================
// Bench Mode (automated multi-board testing — tools/bench-mesh.ps1)
// ============================================================================
// The synthetic source generates this tone instead of taking audio from A2DP,
// so a stream can be produced with no phone in the loop. Audible if a DAC is
// attached, which makes a live run easy to sanity-check by ear.
#define BENCH_TONE_HZ         440
#define BENCH_TONE_AMPLITUDE  6000

// If the source falls behind (blocked for a while), send at most this many
// packets back-to-back to catch up rather than spinning out the whole backlog.
#define BENCH_MAX_CATCHUP_PKTS  14

// A bench source has no speaker, but it has a schedule: each frame is due to
// play this long after it was generated, and its packets' `due` say so, so a
// bench run exercises the clients' sync the way a Bluetooth server does.
// About the prefill -- what a server's own ring holds when it starts.
#define BENCH_PLAY_DELAY_US   136000

// How often each node emits its machine-parsable [BENCH] telemetry line. This
// is the sampling interval for the clock-drift regression, so shorter gives a
// better fit but more serial traffic.
#define BENCH_REPORT_MS  1000

// ============================================================================
// Mode Timeouts
// ============================================================================
// CLIENT returns to DISCOVERY after this many ms of ESP-NOW silence.
#define ESPNOW_SILENCE_TIMEOUT_MS  5000

// How often the periodic status line is printed (DEBUG_LEVEL >= 3).
#define STATUS_INTERVAL_MS  10000

// ============================================================================
// Debug
// ============================================================================
// Not Serial itself: main.cpp's Console, which is the serial port plus a copy
// of what a command run over the mesh prints, for its reply (D16).
#define DEBUG_SERIAL    debugOut
#define DEBUG_BAUD_RATE 115200

// Native USB serial only (S3, C3; main.cpp setup()): transmit buffer bytes,
// and the longest a write may wait for a host that has stopped reading. The
// core's 100 ms default stalled the audio whenever the PC was not reading.
#define USB_SERIAL_TX_BUFFER      4096
#define USB_SERIAL_TX_TIMEOUT_MS  5

// 0=Off 1=Error 2=Warn 3=Info 4=Debug
#define DEBUG_LEVEL 3

#if DEBUG_LEVEL >= 1
#define LOG_ERROR(msg) { DEBUG_SERIAL.print("[ERROR] "); DEBUG_SERIAL.println(msg); }
#else
#define LOG_ERROR(msg)
#endif

#if DEBUG_LEVEL >= 2
#define LOG_WARN(msg)  { DEBUG_SERIAL.print("[WARN]  "); DEBUG_SERIAL.println(msg); }
#else
#define LOG_WARN(msg)
#endif

#if DEBUG_LEVEL >= 3
#define LOG_INFO(msg)  { DEBUG_SERIAL.print("[INFO]  "); DEBUG_SERIAL.println(msg); }
#else
#define LOG_INFO(msg)
#endif

#if DEBUG_LEVEL >= 4
#define LOG_DEBUG(msg) { DEBUG_SERIAL.print("[DEBUG] "); DEBUG_SERIAL.println(msg); }
#else
#define LOG_DEBUG(msg)
#endif

#endif // CONFIG_H
