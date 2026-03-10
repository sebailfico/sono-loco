/**
 * AudioMultiRoom - ESP32 Bluetooth Speaker
 *
 * SIMPLIFIED VERSION: BT A2DP -> I2S only (ESP-NOW/SBC commented out for debugging)
 *
 * Hardware: ESP32 DevKit + PCM5102 DAC + TPA3116 Amplifier
 */

#include <Arduino.h>
#include "config.h"
#include <driver/i2s.h>
#include <math.h>

// Audio Tools framework (minimal - just for types if needed)
// #include "AudioTools.h"
// #include "AudioTools/Communication/ESPNowStream.h"  // COMMENTED OUT - ESP-NOW disabled
// #include "AudioTools/AudioCodecs/CodecSBC.h"        // COMMENTED OUT - SBC disabled

// Bluetooth A2DP Sink
#include "BluetoothA2DPSink.h"

// ============================================================================
// Tone Generation Constants
// ============================================================================

#define TONE_SAMPLE_RATE 44100
#define TONE_AMPLITUDE   500       // Volume (max 32767 for 16-bit) - kept low!

// Musical note frequencies (Hz)
#define NOTE_C5  523
#define NOTE_E5  659
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_C6  1047

// ============================================================================
// Global Objects
// ============================================================================

// Bluetooth A2DP Sink (handles I2S output internally)
BluetoothA2DPSink a2dpSink;

// State tracking
volatile bool btConnected = false;
volatile bool btPreviouslyConnected = false;
volatile bool playConnectSound = false;
volatile bool playDisconnectSound = false;
unsigned long lastStatusPrint = 0;

// ============================================================================
// Tone Generation Functions
// ============================================================================

// Generate and play a single tone
void playTone(uint16_t frequency, uint16_t durationMs, uint16_t amplitude = TONE_AMPLITUDE) {
    const int sampleCount = (TONE_SAMPLE_RATE * durationMs) / 1000;
    const int bufferSize = 512;  // Samples per write (stereo pairs)
    int16_t buffer[bufferSize * 2];  // Stereo: L, R, L, R...

    int samplesWritten = 0;
    while (samplesWritten < sampleCount) {
        int samplesToWrite = min(bufferSize, sampleCount - samplesWritten);

        for (int i = 0; i < samplesToWrite; i++) {
            float t = (float)(samplesWritten + i) / TONE_SAMPLE_RATE;
            int16_t sample = (int16_t)(amplitude * sin(2.0 * M_PI * frequency * t));

            // Apply fade in/out to avoid clicks (first and last 5ms)
            int fadesamples = TONE_SAMPLE_RATE * 5 / 1000;  // 5ms fade
            int pos = samplesWritten + i;
            if (pos < fadesamples) {
                sample = sample * pos / fadesamples;
            } else if (pos > sampleCount - fadesamples) {
                sample = sample * (sampleCount - pos) / fadesamples;
            }

            buffer[i * 2] = sample;      // Left
            buffer[i * 2 + 1] = sample;  // Right
        }

        size_t bytesWritten;
        i2s_write(I2S_NUM_0, buffer, samplesToWrite * 4, &bytesWritten, portMAX_DELAY);
        samplesWritten += samplesToWrite;
    }
}

// Play silence (to clear buffer and avoid pops)
void playSilence(uint16_t durationMs) {
    const int sampleCount = (TONE_SAMPLE_RATE * durationMs) / 1000;
    const int bufferSize = 512;
    int16_t buffer[bufferSize * 2] = {0};

    int samplesWritten = 0;
    while (samplesWritten < sampleCount) {
        int samplesToWrite = min(bufferSize, sampleCount - samplesWritten);
        size_t bytesWritten;
        i2s_write(I2S_NUM_0, buffer, samplesToWrite * 4, &bytesWritten, portMAX_DELAY);
        samplesWritten += samplesToWrite;
    }
}

// Startup sound: "bip-bip"
void playStartupSound() {
    LOG_INFO("Playing startup sound...");
    playTone(NOTE_A5, 100);   // Beep 1
    playSilence(80);
    playTone(NOTE_A5, 100);   // Beep 2
    playSilence(50);
}

// Connect sound: "ta-da" (ascending C-E-G)
void playConnectedSound() {
    LOG_INFO("Playing connect sound...");
    playTone(NOTE_C5, 120);
    playSilence(30);
    playTone(NOTE_E5, 120);
    playSilence(30);
    playTone(NOTE_G5, 180);
    playSilence(50);
}

// Disconnect sound: reversed "ta-da" (descending G-E-C)
void playDisconnectedSound() {
    LOG_INFO("Playing disconnect sound...");
    playTone(NOTE_G5, 120);
    playSilence(30);
    playTone(NOTE_E5, 120);
    playSilence(30);
    playTone(NOTE_C5, 180);
    playSilence(50);
}

// Initialize I2S for tone playback (before BT takes over)
void initI2SForTones() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = TONE_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_0);
}

// Cleanup I2S after tone playback
void deinitI2SForTones() {
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
}

/* ==========================================================================
 * COMMENTED OUT - ESP-NOW / SBC / State Machine
 * ==========================================================================
 *
 * // State Machine
 * enum DeviceMode {
 *     MODE_DISCOVERY,   // Boot: BT discoverable + listening for ESP-NOW
 *     MODE_SERVER,      // Phone connected via BT, forwarding to mesh
 *     MODE_CLIENT       // Receiving audio from mesh via ESP-NOW
 * };
 * volatile DeviceMode currentMode = MODE_DISCOVERY;
 *
 * // I2S output for PCM5102 DAC — used in CLIENT mode only
 * I2SStream i2sOut;
 *
 * // ESP-NOW stream (broadcast mode for mesh)
 * ESPNowStream espNow;
 *
 * // SBC codec instances
 * SBCEncoder sbcEncoder(SBC_SUBBANDS, SBC_BLOCKS, SBC_BITPOOL);
 * SBCDecoder sbcDecoder(8192);
 *
 * // Encoded stream for SERVER mode: SBC encode -> ESP-NOW broadcast
 * EncodedAudioStream encoderStream(&espNow, &sbcEncoder);
 *
 * // Encoded stream for CLIENT mode: ESP-NOW -> SBC decode -> I2S
 * EncodedAudioStream decoderStream(&i2sOut, &sbcDecoder);
 *
 * // StreamCopy for CLIENT mode: reads ESP-NOW, writes to decoder
 * StreamCopy clientCopier(decoderStream, espNow);
 *
 * // Thread-safe ring buffer: BT callback -> main loop
 * BufferRTOS<uint8_t> btRingBuffer(0);
 *
 * // Additional state tracking
 * volatile bool serverModeRequested = false;
 * bool espNowActive = false;
 * bool espNowReceiving = false;
 * bool serverForwardingActive = false;
 * unsigned long lastBtActiveTime = 0;
 * unsigned long lastEspNowDataTime = 0;
 *
 * // Callback stats
 * volatile uint32_t cbCallCount = 0;
 * volatile uint32_t cbTotalBytes = 0;
 * volatile uint32_t cbDroppedBytes = 0;
 * unsigned long lastCbStatTime = 0;
 *
 * // Temp buffer for reading from ring buffer
 * uint8_t serverTempBuf[512];
 *
 * ========================================================================== */

// ============================================================================
// Bluetooth A2DP Callbacks
// ============================================================================

// Called when audio streaming starts/stops (separate from connection state)
void btAudioStateChanged(esp_a2d_audio_state_t state, void *obj) {
    if (state == ESP_A2D_AUDIO_STATE_STARTED) {
        LOG_INFO("BT audio stream STARTED");
    } else if (state == ESP_A2D_AUDIO_STATE_STOPPED) {
        LOG_INFO("BT audio stream STOPPED");
    } else if (state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
        LOG_INFO("BT audio stream SUSPENDED by remote");
    }
}

// Called when BT connection state changes
void btConnectionStateChanged(esp_a2d_connection_state_t state, void *obj) {
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        LOG_INFO("BT device connected!");
        btConnected = true;
        playConnectSound = true;  // Flag for main loop to play sound
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        LOG_INFO("BT device disconnected!");
        btConnected = false;
        playDisconnectSound = true;  // Flag for main loop to play sound
    }
}

/* ==========================================================================
 * COMMENTED OUT - Data callback for ESP-NOW forwarding
 * ==========================================================================
 *
 * void a2dpDataCallback(const uint8_t *data, uint32_t length) {
 *     cbCallCount++;
 *     cbTotalBytes += length;
 *
 *     if (serverForwardingActive) {
 *         int wrote = btRingBuffer.writeArray(data, length);
 *         if (wrote < (int)length) {
 *             cbDroppedBytes += (length - wrote);
 *         }
 *     }
 *
 *     lastBtActiveTime = millis();
 * }
 *
 * ========================================================================== */

/* ==========================================================================
 * COMMENTED OUT - I2S Setup (BT library handles I2S internally)
 * ==========================================================================
 *
 * bool setupI2S() {
 *     LOG_INFO("Configuring I2S for PCM5102...");
 *
 *     auto config = i2sOut.defaultConfig(TX_MODE);
 *     config.sample_rate = BT_SAMPLE_RATE;
 *     config.bits_per_sample = BT_BITS_PER_SAMPLE;
 *     config.channels = BT_CHANNELS;
 *     config.pin_bck = I2S_BCK_PIN;
 *     config.pin_ws = I2S_WS_PIN;
 *     config.pin_data = I2S_DATA_PIN;
 *     config.buffer_size = I2S_BUFFER_SIZE;
 *     config.buffer_count = I2S_BUFFER_COUNT;
 *
 *     if (!i2sOut.begin(config)) {
 *         LOG_ERROR("I2S initialization failed!");
 *         return false;
 *     }
 *
 *     LOG_INFO("I2S configured: 44100Hz 16bit stereo");
 *     return true;
 * }
 *
 * ========================================================================== */

/* ==========================================================================
 * COMMENTED OUT - ESP-NOW Setup
 * ==========================================================================
 *
 * bool setupESPNow() {
 *     LOG_INFO("Initializing ESP-NOW...");
 *     auto cfg = espNow.defaultConfig();
 *     cfg.channel = ESPNOW_CHANNEL;
 *     cfg.buffer_size = ESPNOW_BUFFER_SIZE;
 *     cfg.buffer_count = ESPNOW_BUFFER_COUNT;
 *     cfg.use_send_ack = false;  // Broadcast has no ACKs
 *
 *     if (!espNow.begin(cfg)) {
 *         LOG_ERROR("ESP-NOW initialization failed!");
 *         return false;
 *     }
 *
 *     espNow.addBroadcastPeer();
 *     espNowActive = true;
 *     LOG_INFO("ESP-NOW ready, MAC: " + String(espNow.macAddress()));
 *     return true;
 * }
 *
 * ========================================================================== */

// ============================================================================
// Bluetooth Setup
// ============================================================================

void setupBluetooth() {
    LOG_INFO("Starting Bluetooth A2DP Sink...");

    // Configure BT library's I2S to use our PCM5102 pins
    i2s_pin_config_t pin_config = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    a2dpSink.set_pin_config(pin_config);

    // BT library handles I2S output internally - no custom data callback needed
    // a2dpSink.set_stream_reader(a2dpDataCallback, true);  // COMMENTED OUT - not forwarding to ESP-NOW

    a2dpSink.set_on_connection_state_changed(btConnectionStateChanged);
    a2dpSink.set_on_audio_state_changed(btAudioStateChanged);
    a2dpSink.set_auto_reconnect(false);
    a2dpSink.set_volume(VOLUME_DEFAULT);
    a2dpSink.start(BT_DEVICE_NAME);

    LOG_INFO("BT discoverable as: " BT_DEVICE_NAME);
}

/* ==========================================================================
 * COMMENTED OUT - Mode Transitions (ESP-NOW disabled)
 * ==========================================================================
 *
 * void enterDiscoveryMode() {
 *     LOG_INFO("=== Entering DISCOVERY mode ===");
 *     currentMode = MODE_DISCOVERY;
 *     serverForwardingActive = false;
 *     espNowReceiving = false;
 *     btConnected = false;
 *
 *     if (espNowActive) {
 *         espNow.end();
 *         espNowActive = false;
 *     }
 *     if (!setupESPNow()) {
 *         LOG_WARN("ESP-NOW init failed in DISCOVERY");
 *     }
 *
 *     LOG_INFO("Discovery: BT discoverable + ESP-NOW listening");
 *     LOG_INFO("Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
 * }
 *
 * void enterServerMode() {
 *     LOG_INFO("=== Entering SERVER mode ===");
 *     currentMode = MODE_SERVER;
 *     serverForwardingActive = false;
 *
 *     if (espNowActive) {
 *         LOG_INFO("Stopping ESP-NOW to free BT heap...");
 *         espNow.end();
 *         espNowActive = false;
 *     }
 *
 *     LOG_INFO("Server: BT audio playing locally");
 *     LOG_INFO("Free Heap: " + String(ESP.getFreeHeap()) + " (max contiguous: " + String(ESP.getMaxAllocHeap()) + ")");
 * }
 *
 * void enterClientMode() {
 *     LOG_INFO("=== Entering CLIENT mode ===");
 *
 *     LOG_INFO("Stopping Bluetooth to save resources...");
 *     a2dpSink.end(true);
 *     btStop();
 *
 *     currentMode = MODE_CLIENT;
 *
 *     if (!setupI2S()) {
 *         LOG_ERROR("I2S init failed in CLIENT mode!");
 *         return;
 *     }
 *
 *     decoderStream.begin();
 *     clientCopier.setRetry(0);
 *
 *     LOG_INFO("Client: receiving ESP-NOW audio");
 *     LOG_INFO("Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
 * }
 *
 * ========================================================================== */

// ============================================================================
// Status Reporting
// ============================================================================

void printStatus() {
    if (millis() - lastStatusPrint < 10000) return;
    lastStatusPrint = millis();

    LOG_INFO("=== Status ===");
    LOG_INFO("BT connected: " + String(btConnected ? "Yes" : "No"));
    LOG_INFO("Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
}

// ============================================================================
// Main Setup
// ============================================================================

void setup() {
    DEBUG_SERIAL.begin(DEBUG_BAUD_RATE);
    delay(1000);

    DEBUG_SERIAL.println();
    DEBUG_SERIAL.println("================================");
    DEBUG_SERIAL.println("  AudioMultiRoom - BT ONLY MODE");
    DEBUG_SERIAL.println("  Room: " ROOM_NAME);
    DEBUG_SERIAL.println("================================");
    DEBUG_SERIAL.println();

    LOG_INFO("ESP32 Chip: " + String(ESP.getChipModel()));
    LOG_INFO("CPU Freq: " + String(ESP.getCpuFreqMHz()) + " MHz");
    LOG_INFO("Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
    DEBUG_SERIAL.println();

    // Play startup sound before BT takes over I2S
    initI2SForTones();
    playStartupSound();
    deinitI2SForTones();

    // Start Bluetooth A2DP Sink (handles I2S internally)
    setupBluetooth();

    DEBUG_SERIAL.println();
    LOG_INFO("Setup complete! Waiting for BT connection...");
    LOG_INFO("BT discoverable as: " BT_DEVICE_NAME);
    DEBUG_SERIAL.println();
}

// ============================================================================
// Main Loop
// ============================================================================

void loop() {
    // Handle connect/disconnect sounds
    // Note: A2DP library has I2S configured, we can write directly to I2S_NUM_0
    if (playConnectSound) {
        playConnectSound = false;
        delay(100);  // Brief delay to let BT settle
        playConnectedSound();
    }

    if (playDisconnectSound) {
        playDisconnectSound = false;
        delay(100);  // Brief delay
        playDisconnectedSound();
    }

    // Print status periodically for debugging
    #if DEBUG_LEVEL >= 3
        printStatus();
    #endif

    delay(100);
}

/* ==========================================================================
 * COMMENTED OUT - Original state machine loop
 * ==========================================================================
 *
 * void loop() {
 *     switch (currentMode) {
 *
 *         case MODE_DISCOVERY: {
 *             if (serverModeRequested) {
 *                 serverModeRequested = false;
 *                 enterServerMode();
 *                 break;
 *             }
 *             if (espNow.available() > 0) {
 *                 LOG_INFO("ESP-NOW audio detected! Becoming CLIENT...");
 *                 enterClientMode();
 *             }
 *             delay(10);
 *             break;
 *         }
 *
 *         case MODE_SERVER: {
 *             int available = btRingBuffer.available();
 *             while (available > 0) {
 *                 int toRead = min(available, (int)sizeof(serverTempBuf));
 *                 int bytesRead = btRingBuffer.readArray(serverTempBuf, toRead);
 *                 if (bytesRead > 0) {
 *                     int sent = encoderStream.write(serverTempBuf, bytesRead);
 *                     if (sent < bytesRead) {
 *                         LOG_WARN("encoderStream.write() short: sent " + String(sent) + "/" + String(bytesRead));
 *                     }
 *                 }
 *                 available = btRingBuffer.available();
 *             }
 *
 *             if (millis() - lastCbStatTime >= 5000) {
 *                 lastCbStatTime = millis();
 *                 LOG_INFO("BT cb stats — calls: " + String(cbCallCount)
 *                     + ", bytes: " + String(cbTotalBytes)
 *                     + ", dropped: " + String(cbDroppedBytes)
 *                     + ", ringbuf: " + String(btRingBuffer.available()) + "/" + String(BT_RINGBUF_SIZE));
 *                 cbCallCount = 0; cbTotalBytes = 0; cbDroppedBytes = 0;
 *             }
 *
 *             if (!btConnected && (millis() - lastBtActiveTime > BT_DISCONNECT_TIMEOUT_MS)) {
 *                 LOG_INFO("BT disconnected, returning to DISCOVERY...");
 *                 serverForwardingActive = false;
 *                 encoderStream.end();
 *                 enterDiscoveryMode();
 *             }
 *
 *             delay(1);
 *             break;
 *         }
 *
 *         case MODE_CLIENT: {
 *             if (espNow.available() > 0) {
 *                 clientCopier.copy();
 *                 lastEspNowDataTime = millis();
 *                 espNowReceiving = true;
 *             }
 *
 *             if (espNowReceiving && (millis() - lastEspNowDataTime > ESPNOW_SILENCE_TIMEOUT_MS)) {
 *                 LOG_INFO("ESP-NOW silence, returning to DISCOVERY...");
 *                 decoderStream.end();
 *                 i2sOut.end();
 *                 espNowReceiving = false;
 *
 *                 LOG_INFO("Restarting Bluetooth...");
 *                 btStart();
 *                 setupBluetooth();
 *                 enterDiscoveryMode();
 *             }
 *
 *             delay(1);
 *             break;
 *         }
 *     }
 *
 *     #if DEBUG_LEVEL >= 3
 *         printStatus();
 *     #endif
 * }
 *
 * ========================================================================== */
