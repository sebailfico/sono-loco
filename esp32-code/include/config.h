#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// Room / Device Identification
// ============================================================================
// Visible as the Bluetooth speaker name.
//
// Set it per node in platformio.ini (-DROOM_NAME='"Kitchen"'), not here, so the
// source tree stays identical for every board and you flash an environment
// rather than an edited file. This fallback only applies to a build that does
// not define it.
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
// audible on its DAC. Still provisional on the S3, which has no DAC yet.
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
#define BT_DEVICE_NAME   ROOM_NAME
#define BT_SAMPLE_RATE   44100
#define VOLUME_DEFAULT   1       // 0–127; keep low, TPA3116 has high gain

// The server's local I2S DMA ring. The A2DP library writes each decoded packet
// to I2S from the Bluetooth task and blocks until its tail fits, so between
// packets this ring is all that plays -- whatever the task spends waiting for
// the next packet, decoding it and forwarding it to the mesh must fit inside
// it, or the DMA plays zeros. The library's default is 8 x 64 frames, 11.6 ms:
// half of one 23.2 ms packet from Windows. Measured 2026-09-28 the task spent
// 5-10 ms between packets in steady play, 10-27 ms around the start of a
// stream, plus up to 5 ms in the forwarding callback. 8 x 256 is 46 ms, costs
// 6 KB more internal DRAM, and a zeroed buffer on an underrun is 5.8 ms
// instead of 1.45. A ring deeper than a packet only helps while it is full,
// so every stream starts by filling it with silence (serverPrefill); the
// local output is then the ring's depth late. That latency is not a cost:
// the server's local output will have to be delayed to meet the clients'
// ~137 ms anyway.
#define SERVER_DMA_BUF_COUNT  8
#define SERVER_DMA_BUF_LEN    256

// ============================================================================
// Notification Tones (startup / connect / disconnect)
// ============================================================================
// The tones use BT_SAMPLE_RATE directly. Connect/disconnect tones are written
// into an I2S driver that A2DP configured, so the tone generator cannot pick its
// own rate — there is deliberately no separate TONE_SAMPLE_RATE to drift from it.
#define TONE_AMPLITUDE   500     // peak amplitude of a 16-bit tone sample
#define TONE_FADE_MS     5       // ramp in/out, kills the click at tone edges

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

// Long-press-to-pair button. The devkit BOOT button on every board here, since
// it is the only one that exists; set to -1 to disable the button entirely and
// pair over serial only.
//
// It is a long press *while running*, never "hold it down at boot": BOOT is a
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

// How long the button must be held to start pairing. Long enough that it cannot
// be confused with the reset-adjacent fumbling that BOOT normally sees.
#define MESH_PAIR_HOLD_MS     3000

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
// before the silence plays -- well inside the ~44 ms a client keeps
// buffered. Sent in every packet, so a server's `D<n>` changes it for the
// whole mesh at once.
#define MESH_REDUNDANCY_DISTANCE  1

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
#define CLIENT_DMA_BUF_COUNT  8
#define CLIENT_DMA_BUF_LEN    256

// Bytes the DMA ring can swallow when completely empty: 8 * 256 * 4 = 8192.
#define CLIENT_DMA_CAPACITY_BYTES (CLIENT_DMA_BUF_COUNT * CLIENT_DMA_BUF_LEN * CLIENT_FRAME_BYTES)

// Minimum bytes in the jitter buffer before I2S output starts (~91 ms).
//
// This MUST exceed CLIENT_DMA_CAPACITY_BYTES, and the first hardware run is why:
// at 2000 bytes it was below the DMA capacity of the then 8-buffer ring, so
// every time the buffer armed, the DMA swallowed the whole prefill in one pass
// and the buffer immediately ran dry -- 24 underruns per second, forever, and a
// jitter buffer that never held more than a fraction of its intended depth. The
// audio survived on DMA buffering alone. A static_assert in main.cpp enforces
// the relationship now.
#define JITTER_PREFILL     16000

// Stereo frames handed to I2S per loop() pass.
#define CLIENT_BATCH       128

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
// timing. Capped so one long outage can't flood the buffer with silence: 8
// blocks is 21 ms, about what 4 packets of the old format were.
#define MAX_GAP_FILL_PKTS  8

// A sequence number this far from the expected one is treated as a stream
// restart, not as a gap. Sequence numbers are uint16_t, so a duplicate or
// reordered frame computes as a "gap" of ~65535 — filling that as loss would
// charge tens of thousands to the lost counter and inject bogus silence.
#define SEQ_RESYNC_THRESHOLD  64

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
#define DEBUG_SERIAL    Serial
#define DEBUG_BAUD_RATE 115200

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
