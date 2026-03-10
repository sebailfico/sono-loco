#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// Room Identification
// ============================================================================
// Unique name for this device (visible as Bluetooth speaker name)
#define ROOM_NAME "Room1"

// ============================================================================
// I2S / PCM5102 DAC Configuration
// ============================================================================
// PCM5102 wiring to ESP32:
//   PCM5102 BCK  -> ESP32 GPIO 26 (Bit Clock)
//   PCM5102 DIN  -> ESP32 GPIO 25 (Data)
//   PCM5102 LCK  -> ESP32 GPIO 22 (Word Select / LR Clock)
//   PCM5102 SCK  -> GND (or leave floating, PCM5102 generates internally)
//   PCM5102 FMT  -> GND (I2S format)
//   PCM5102 XSMT -> 3.3V (soft mute off) or GPIO for mute control
//   PCM5102 FLT  -> GND (normal latency)
//   PCM5102 DEMP -> GND (de-emphasis off)

#define I2S_BCK_PIN 26  // Bit Clock
#define I2S_DATA_PIN 25 // Data Out
#define I2S_WS_PIN 22   // Word Select (LR Clock)

// I2S buffer configuration
#define I2S_BUFFER_SIZE 512
#define I2S_BUFFER_COUNT 20

// ============================================================================
// Bluetooth A2DP Configuration
// ============================================================================
// Bluetooth device name (visible when pairing from phone)
#define BT_DEVICE_NAME ROOM_NAME

// A2DP audio format (fixed by Bluetooth standard)
#define BT_SAMPLE_RATE 44100
#define BT_BITS_PER_SAMPLE 16
#define BT_CHANNELS 2

// ============================================================================
// ESP-NOW Mesh Configuration
// ============================================================================
// ESP-NOW channel (0 = auto)
#define ESPNOW_CHANNEL 0

// ESP-NOW receive buffer: 250 bytes * 400 frames = 100KB
// Only allocated on first received packet (lazy allocation)
#define ESPNOW_BUFFER_SIZE 250   // ESP_NOW_MAX_DATA_LEN
#define ESPNOW_BUFFER_COUNT 400

// ============================================================================
// SBC Codec Configuration (for ESP-NOW audio transport)
// ============================================================================
// subbands=8, blocks=16, bitpool=32 → ~44 KB/s at 44100Hz stereo
// Fits well within ESP-NOW's ~100 KB/s throughput
#define SBC_SUBBANDS 8
#define SBC_BLOCKS 16
#define SBC_BITPOOL 32

// ============================================================================
// Ring Buffer (BT callback -> main loop for ESP-NOW forwarding)
// ============================================================================
// 44100 Hz * 2 ch * 2 bytes * 0.046 sec ≈ 8KB
// NOTE: heap is fragmented after BT+ESP-NOW init; largest contiguous block <16KB
#define BT_RINGBUF_SIZE 8192

// ============================================================================
// Mode Transition Timeouts
// ============================================================================
// How long (ms) of ESP-NOW silence before CLIENT returns to DISCOVERY
#define ESPNOW_SILENCE_TIMEOUT_MS 5000  // 5 seconds

// How long (ms) after BT disconnect before SERVER returns to DISCOVERY
#define BT_DISCONNECT_TIMEOUT_MS 3000   // 3 seconds

// ============================================================================
// Audio Control Configuration
// ============================================================================
#define VOLUME_DEFAULT 1   // 0-127; 13 ≈ 10% amplitude (~90% reduction for loud TPA3116 setups)
#define VOLUME_MIN 0
#define VOLUME_MAX 127

// ============================================================================
// Debug Configuration
// ============================================================================
#define DEBUG_SERIAL Serial
#define DEBUG_BAUD_RATE 115200

// Debug levels: 0=Off, 1=Error, 2=Warn, 3=Info, 4=Debug, 5=Verbose
#define DEBUG_LEVEL 3

// Macros for debug output
#if DEBUG_LEVEL >= 1
#define LOG_ERROR(...)                     \
    {                                      \
        DEBUG_SERIAL.print("[ERROR] ");    \
        DEBUG_SERIAL.println(__VA_ARGS__); \
    }
#else
#define LOG_ERROR(...)
#endif

#if DEBUG_LEVEL >= 2
#define LOG_WARN(...)                      \
    {                                      \
        DEBUG_SERIAL.print("[WARN]  ");    \
        DEBUG_SERIAL.println(__VA_ARGS__); \
    }
#else
#define LOG_WARN(...)
#endif

#if DEBUG_LEVEL >= 3
#define LOG_INFO(...)                      \
    {                                      \
        DEBUG_SERIAL.print("[INFO]  ");    \
        DEBUG_SERIAL.println(__VA_ARGS__); \
    }
#else
#define LOG_INFO(...)
#endif

#if DEBUG_LEVEL >= 4
#define LOG_DEBUG(...)                     \
    {                                      \
        DEBUG_SERIAL.print("[DEBUG] ");    \
        DEBUG_SERIAL.println(__VA_ARGS__); \
    }
#else
#define LOG_DEBUG(...)
#endif

#if DEBUG_LEVEL >= 5
#define LOG_VERBOSE(...)                   \
    {                                      \
        DEBUG_SERIAL.print("[VERB]  ");    \
        DEBUG_SERIAL.println(__VA_ARGS__); \
    }
#else
#define LOG_VERBOSE(...)
#endif

// ============================================================================
// Future: Presence Detection Configuration (ESPectre)
// ============================================================================
#define PRESENCE_CHECK_INTERVAL_MS 200 // 5 Hz polling
#define ABSENCE_TIMEOUT_MS 30000       // 30 seconds before pause

#endif // CONFIG_H
