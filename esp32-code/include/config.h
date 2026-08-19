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
// I2S / PCM5102 DAC Pin Configuration
// ============================================================================
// PCM5102 wiring to ESP32:
//   PCM5102 BCK  -> GPIO 26
//   PCM5102 DIN  -> GPIO 25
//   PCM5102 LCK  -> GPIO 22
//   PCM5102 SCK  -> GND        (PCM5102 generates clock internally)
//   PCM5102 FMT  -> GND        (standard I2S format)
//   PCM5102 XSMT -> 3.3V       (soft mute off)
//   PCM5102 FLT  -> GND        (normal latency)
//   PCM5102 DEMP -> GND        (de-emphasis off)

#define I2S_BCK_PIN  26
#define I2S_DATA_PIN 25
#define I2S_WS_PIN   22

// ============================================================================
// Bluetooth A2DP (SERVER mode — BT Classic nodes only)
// ============================================================================
#define BT_DEVICE_NAME   ROOM_NAME
#define BT_SAMPLE_RATE   44100
#define VOLUME_DEFAULT   1       // 0–127; keep low, TPA3116 has high gain

// ============================================================================
// Notification Tones (startup / connect / disconnect)
// ============================================================================
// The tones use BT_SAMPLE_RATE directly. Connect/disconnect tones are written
// into an I2S driver that A2DP configured, so the tone generator cannot pick its
// own rate — there is deliberately no separate TONE_SAMPLE_RATE to drift from it.
#define TONE_AMPLITUDE   500     // peak amplitude of a 16-bit tone sample
#define TONE_FADE_MS     5       // ramp in/out, kills the click at tone edges

// ============================================================================
// ESP-NOW Mesh
// ============================================================================
// Fixed channel — must be the same on every node.
#define ESPNOW_CHANNEL       1

// Audio payload per packet (bytes). Must be ≤ 250 (ESP-NOW max).
// At 22050Hz mono 16-bit: 200 bytes = ~4.5ms of audio per packet (~220 pkt/s)
#define ESPNOW_PAYLOAD_SIZE  200

// FreeRTOS queue depth for the ESP-NOW TX task (packets buffered before dropping)
#define ESPNOW_TX_QUEUE_DEPTH  32

// Hold ESP-NOW TX for this long after BT audio starts, so the A2DP pipeline has
// settled before the radio starts competing with it.
#define TX_WARMUP_MS  1000

// --- WiFi driver buffer counts (see setupESPNow) -----------------------------
// Receive side carries ~220 packets/s continuously. These buffers are DMA-capable
// *internal* DRAM — PSRAM cannot back them, so trimming them is the only way to
// claw back DRAM, and trimming them too far drops audio. 10 is the IDF default;
// each buffer costs roughly 1.6 KB, so this is ~16 KB of DRAM.
#define WIFI_STATIC_RX_BUFFERS   10

// Transmit side is gated on the send-complete semaphore, so at most one frame is
// ever in flight. 4 is plenty; the default 32 would just waste heap.
#define WIFI_DYNAMIC_TX_BUFFERS  4

// ============================================================================
// Audio Downsampling (SERVER → CLIENT over ESP-NOW)
// ============================================================================
// BT A2DP delivers 44100Hz stereo 16-bit = 176 KB/s
// We downsample to 22050Hz mono 16-bit = 44 KB/s before broadcasting.
// Reduction: 2x from halving sample rate + 2x from stereo→mono = 4x total.
#define CLIENT_SAMPLE_RATE  22050

// ============================================================================
// Jitter Buffer (CLIENT mode)
// ============================================================================
// Absorbs network timing variation before writing to I2S.
// 8192 bytes at 22050Hz mono 16-bit ≈ 185ms of audio.
// MUST be a power of two — the ring uses masking, not modulo (static_assert
// in main.cpp enforces this).
#define JITTER_BUF_SIZE    8192

// Client I2S DMA ring. dma_buf_len counts stereo frames, so the ring holds
// CLIENT_DMA_BUF_COUNT * CLIENT_DMA_BUF_LEN frames, and each frame consumes two
// bytes of mono from the jitter buffer.
#define CLIENT_DMA_BUF_COUNT  4
#define CLIENT_DMA_BUF_LEN    256

// Bytes of mono audio the DMA ring can swallow when completely empty:
// 4 * 256 frames * 2 bytes = 2048 (~46 ms).
#define CLIENT_DMA_CAPACITY_BYTES (CLIENT_DMA_BUF_COUNT * CLIENT_DMA_BUF_LEN * 2)

// Minimum bytes in the jitter buffer before I2S output starts (~91 ms).
//
// This MUST exceed CLIENT_DMA_CAPACITY_BYTES, and the first hardware run is why:
// at 2000 bytes it was below the DMA capacity of the then 8-buffer ring, so
// every time the buffer armed, the DMA swallowed the whole prefill in one pass
// and the buffer immediately ran dry -- 24 underruns per second, forever, and a
// jitter buffer that never held more than a fraction of its intended depth. The
// audio survived on DMA buffering alone. A static_assert in main.cpp enforces
// the relationship now.
#define JITTER_PREFILL     4000

// Mono samples handed to I2S per loop() pass.
#define CLIENT_BATCH       128

// Lost packets are replaced with an equal amount of silence to keep playback
// timing. Capped so one long outage can't flood the buffer with silence.
#define MAX_GAP_FILL_PKTS  4

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
#define BENCH_MAX_CATCHUP_PKTS  8

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
