/**
 * SonoLoco — Unified Multi-Room Audio Firmware
 *
 * Every node runs this same firmware. Roles are negotiated at runtime:
 *
 *   DISCOVERY → phone connects via BT  → SERVER (plays locally + ESP-NOW broadcast)
 *   DISCOVERY → ESP-NOW audio detected → CLIENT (receives + plays via I2S)
 *   SERVER    → phone disconnects      → DISCOVERY
 *   CLIENT    → ESP-NOW silent 5s      → DISCOVERY
 *
 * Compile with -DENABLE_BLUETOOTH=1 for nodes with BT Classic (ESP32 WROOM/WROVER).
 * Nodes without BT (e.g. ESP32-S3) will always operate as CLIENT.
 *
 * Hardware: ESP32 DevKit + PCM5102 DAC (I2S) + TPA3116 Amplifier
 */

#include <Arduino.h>
#include "config.h"
#include <driver/i2s.h>
#include <math.h>
#include <string.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifdef ENABLE_BLUETOOTH
#include "BluetoothA2DPSink.h"
#endif

// ============================================================================
// State Machine
// ============================================================================

enum DeviceMode { MODE_DISCOVERY, MODE_SERVER, MODE_CLIENT };
volatile DeviceMode currentMode = MODE_DISCOVERY;

// ============================================================================
// I2S write helper
// ============================================================================

/**
 * i2s_write() is allowed to accept FEWER bytes than requested — it returns what
 * it actually queued in `bytes_written` and that is frequently less than the
 * full buffer when the DMA ring is busy. Callers that ignore it silently throw
 * audio away. This helper loops until everything is out, or gives up if the DMA
 * is not draining at all (e.g. another owner has taken over the peripheral).
 *
 * Returns the number of bytes actually written.
 */
static size_t i2sWriteAll(const void *src, size_t bytes, TickType_t timeout) {
    const uint8_t *p = (const uint8_t *)src;
    size_t done = 0;
    while (done < bytes) {
        size_t bw = 0;
        if (i2s_write(I2S_NUM_0, p + done, bytes - done, &bw, timeout) != ESP_OK) break;
        if (bw == 0) break;   // DMA not draining — bail rather than spin forever
        done += bw;
    }
    return done;
}

// ============================================================================
// Tone Generation (startup / connect / disconnect sounds)
// ============================================================================

#define NOTE_C5  523
#define NOTE_E5  659
#define NOTE_G5  784
#define NOTE_A5  880

void playTone(uint16_t freq, uint16_t durationMs, uint16_t amp = TONE_AMPLITUDE) {
    const int total = (TONE_SAMPLE_RATE * durationMs) / 1000;
    const int chunk = 512;
    const int fade  = TONE_SAMPLE_RATE * TONE_FADE_MS / 1000;
    int16_t buf[chunk * 2];
    int written = 0;

    while (written < total) {
        int n = min(chunk, total - written);

        for (int i = 0; i < n; i++) {
            float t = (float)(written + i) / TONE_SAMPLE_RATE;
            int16_t s = (int16_t)(amp * sinf(2.0f * M_PI * freq * t));
            int pos = written + i;
            if (pos < fade)              s = s * pos / fade;
            else if (pos > total - fade) s = s * (total - pos) / fade;
            buf[i * 2]     = s;
            buf[i * 2 + 1] = s;
        }

        size_t bw = i2sWriteAll(buf, n * 4, pdMS_TO_TICKS(100));
        if (bw < (size_t)n * 4) return;   // I2S is not accepting — abort the tone
        written += n;
    }
}

void playSilence(uint16_t durationMs) {
    const int total = (TONE_SAMPLE_RATE * durationMs) / 1000;
    const int chunk = 512;
    int16_t buf[chunk * 2] = {0};
    int written = 0;

    while (written < total) {
        int n = min(chunk, total - written);
        size_t bw = i2sWriteAll(buf, n * 4, pdMS_TO_TICKS(100));
        if (bw < (size_t)n * 4) return;
        written += n;
    }
}

void playStartupSound() {
    playTone(NOTE_A5, 100); playSilence(80);
    playTone(NOTE_A5, 100); playSilence(50);
}

void playConnectedSound() {
    playTone(NOTE_C5, 120); playSilence(30);
    playTone(NOTE_E5, 120); playSilence(30);
    playTone(NOTE_G5, 180); playSilence(50);
}

void playDisconnectedSound() {
    playTone(NOTE_G5, 120); playSilence(30);
    playTone(NOTE_E5, 120); playSilence(30);
    playTone(NOTE_C5, 180); playSilence(50);
}

void initI2SForTones() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = TONE_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = true
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        LOG_ERROR("I2S install (tones) failed: " + String(esp_err_to_name(err)));
        return;
    }
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);
}

void deinitI2SForTones() {
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
}

// ============================================================================
// ESP-NOW — shared by all nodes
// ============================================================================

#define ESPNOW_HEADER_SIZE 4   // seq (2) + len (2)

typedef struct __attribute__((packed)) {
    uint16_t seq;
    uint16_t len;
    uint8_t  data[ESPNOW_PAYLOAD_SIZE];
} AudioPacket;  // 204 bytes total, well under the 250-byte ESP-NOW limit

static_assert(ESPNOW_HEADER_SIZE + ESPNOW_PAYLOAD_SIZE <= 250,
              "ESP-NOW cannot send more than 250 bytes per frame");
// The accumulator fills two bytes at a time and the RX side masks off odd byte
// counts, so an odd payload size would either overrun accumBuf or truncate a
// sample on every packet.
static_assert(ESPNOW_PAYLOAD_SIZE % 2 == 0,
              "ESPNOW_PAYLOAD_SIZE must hold whole 16-bit samples");

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- TX (SERVER mode) ---
static QueueHandle_t     txQueue      = nullptr;
static SemaphoreHandle_t txDone       = nullptr;   // radio is free for the next frame
static uint16_t          txSeq        = 0;
static uint8_t           accumBuf[ESPNOW_PAYLOAD_SIZE];
static int               accumLen     = 0;
static volatile bool     txReady      = false;   // true while BT audio is streaming
static unsigned long     audioStartMs = 0;       // when BT audio last started

// TX counters — incremented from callbacks, reported from loop(). Never log
// from the send callback itself: it fires ~220x/s and a blocking Serial write
// at that rate causes the very dropouts it would be reporting.
static volatile uint32_t txQueueFull  = 0;
static volatile uint32_t txSendErr    = 0;
static volatile uint32_t txRadioFail  = 0;

// --- RX (CLIENT mode) ---
static_assert((JITTER_BUF_SIZE & (JITTER_BUF_SIZE - 1)) == 0,
              "JITTER_BUF_SIZE must be a power of two");
#define JITTER_MASK (JITTER_BUF_SIZE - 1)

static uint8_t          jBuf[JITTER_BUF_SIZE];
static volatile int     jWrite   = 0;
static volatile int     jRead    = 0;
static volatile bool    jReady   = false;   // true once prefill threshold is met
static volatile bool    rxActive = false;   // set by recv callback, triggers mode switch
static volatile unsigned long lastRxMs = 0;
static volatile uint32_t rxCount   = 0;
static volatile uint32_t rxDropped = 0;     // packets lost per sequence numbers
static volatile uint32_t rxOverflow = 0;    // packets dropped, jitter buffer full
static volatile uint32_t rxUnderrun = 0;    // times playback outran the buffer
static volatile uint32_t rxDupe     = 0;    // exact retransmits, discarded
static volatile uint32_t rxResync   = 0;    // sequence jumped — stream restarted
static uint16_t         lastSeq  = 0;
static bool             firstPkt = true;

// Lock onto the first node we hear so two simultaneous servers can never
// interleave their streams into one jitter buffer.
static uint8_t          lockedSender[6] = {0};
static volatile bool    senderLocked    = false;

static inline int jFill() {
    int f = jWrite - jRead;
    return f < 0 ? f + JITTER_BUF_SIZE : f;
}

static inline int jFree() {
    return JITTER_BUF_SIZE - 1 - jFill();
}

/**
 * Push a whole block or nothing.
 *
 * The previous implementation copied byte-by-byte and skipped individual bytes
 * once the ring was full. Dropping an odd number of bytes shifts every later
 * 16-bit sample by one byte — L/R swap plus each sample assembled from two
 * different sample halves — and it never re-aligns. All-or-nothing keeps the
 * framing intact; a dropped packet is a 4.5 ms glitch, a shifted stream is noise.
 */
static bool jPushBlock(const uint8_t *data, int len) {
    if (len <= 0) return true;
    if (jFree() < len) return false;
    int w = jWrite;
    int first = min(len, JITTER_BUF_SIZE - w);
    memcpy(jBuf + w, data, first);
    if (len > first) memcpy(jBuf, data + first, len - first);
    jWrite = (w + len) & JITTER_MASK;
    return true;
}

static bool jPushSilence(int len) {
    if (len <= 0) return true;
    if (jFree() < len) return false;
    int w = jWrite;
    int first = min(len, JITTER_BUF_SIZE - w);
    memset(jBuf + w, 0, first);
    if (len > first) memset(jBuf, 0, len - first);
    jWrite = (w + len) & JITTER_MASK;
    return true;
}

/** Copy out without consuming — the caller advances only what I2S accepted. */
static bool jPeek(uint8_t *out, int len) {
    if (jFill() < len) return false;
    int r = jRead;
    int first = min(len, JITTER_BUF_SIZE - r);
    memcpy(out, jBuf + r, first);
    if (len > first) memcpy(out + first, jBuf, len - first);
    return true;
}

static inline void jAdvance(int len) {
    jRead = (jRead + len) & JITTER_MASK;
}

static void espnowTxTask(void *) {
    AudioPacket pkt;
    while (true) {
        if (xQueueReceive(txQueue, &pkt, portMAX_DELAY) != pdTRUE) continue;

        // Wait for the previous frame to actually leave the radio. Firing
        // esp_now_send() back-to-back at ~220 pkt/s returns
        // ESP_ERR_ESPNOW_NO_MEM and drops audio without any indication.
        xSemaphoreTake(txDone, pdMS_TO_TICKS(50));

        esp_err_t err = esp_now_send(BROADCAST_ADDR, (uint8_t *)&pkt,
                                     ESPNOW_HEADER_SIZE + pkt.len);
        if (err != ESP_OK) {
            txSendErr++;
            xSemaphoreGive(txDone);   // no callback will come; release the gate
        }
    }
}

static void onEspNowSent(const uint8_t *, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) txRadioFail++;
    if (txDone) xSemaphoreGive(txDone);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    const uint8_t *srcMac = info->src_addr;
#else
static void onEspNowRecv(const uint8_t *srcMac, const uint8_t *data, int len) {
#endif
    if (len < ESPNOW_HEADER_SIZE) return;

    // A SERVER is the source, not a listener. Bailing before the lock below
    // matters: locking here would pin lockedSender to whatever stray node we
    // happened to hear while serving, and that lock outlives SERVER mode.
    if (currentMode == MODE_SERVER) return;

    if (!senderLocked) {
        memcpy(lockedSender, srcMac, 6);
        senderLocked = true;
    } else if (memcmp(lockedSender, srcMac, 6) != 0) {
        return;   // a second server is broadcasting — ignore it
    }

    // The received buffer has no alignment guarantee; copy the header out
    // instead of casting to a struct pointer.
    uint16_t seq, plen;
    memcpy(&seq,  data,     2);
    memcpy(&plen, data + 2, 2);

    // Trust the wire for nothing: clamp to what was actually received, to our
    // own payload limit, and to an even byte count so 16-bit framing survives.
    int n = (int)plen;
    int avail = len - ESPNOW_HEADER_SIZE;
    if (n > avail)                n = avail;
    if (n > ESPNOW_PAYLOAD_SIZE)  n = ESPNOW_PAYLOAD_SIZE;
    n &= ~1;
    if (n <= 0) return;

    lastRxMs = millis();
    rxCount++;

    if (currentMode == MODE_DISCOVERY) {
        rxActive = true;   // signal main loop to switch to CLIENT
        return;
    }

    if (currentMode != MODE_CLIENT) return;

    if (firstPkt) {
        firstPkt = false;
    } else if (seq == lastSeq) {
        rxDupe++;
        return;   // exact retransmit — playing it twice is an audible stutter
    } else {
        uint16_t gap = (uint16_t)(seq - lastSeq - 1);
        if (gap >= SEQ_RESYNC_THRESHOLD) {
            // `gap` is unsigned, so any packet *behind* lastSeq — a reordered
            // frame, or a server that rebooted and restarted its counter at 0 —
            // computes as ~65535. Treating that as loss used to add 65535 to
            // `lost` and splice MAX_GAP_FILL_PKTS of silence into the stream.
            // Just re-baseline on the new sequence and play the payload.
            rxResync++;
        } else if (gap > 0) {
            rxDropped += gap;
            // Substitute silence for the lost packets so playback keeps its
            // timing instead of splicing the stream shorter on every loss.
            int fillPkts = min((int)gap, MAX_GAP_FILL_PKTS);
            if (!jPushSilence(fillPkts * ESPNOW_PAYLOAD_SIZE)) rxOverflow++;
        }
    }
    lastSeq = seq;

    if (!jPushBlock(data + ESPNOW_HEADER_SIZE, n)) rxOverflow++;
}

static bool espnowActive = false;

static void setupESPNow() {
    // BT Classic + WiFi simultaneously requires PSRAM on ESP32.
    // Without PSRAM, WiFi takes ~80KB of the only ~215KB available DRAM heap,
    // leaving the BT stack unable to allocate L2CAP/AVDTP buffers → crash.
    //
    // Guard: only init WiFi if PSRAM is present OR BT is not compiled in.
    // - WROOM (no PSRAM, BT enabled)  → skip WiFi; acts as BT speaker only
    // - S3    (PSRAM,  BT disabled)   → init WiFi; acts as ESP-NOW client
    // - WROVER (PSRAM, BT enabled)    → init WiFi; fully interchangeable
#ifdef ENABLE_BLUETOOTH
    if (ESP.getPsramSize() == 0) {
        LOG_WARN("No PSRAM detected — WiFi/ESP-NOW disabled to protect BT heap.");
        LOG_WARN("This node will play locally only (no mesh). Upgrade to WROVER for full mesh.");
        return;
    }
#endif

    // esp_wifi_init() needs the netif layer and a default event loop in place.
    // Both are no-ops (ESP_ERR_INVALID_STATE) if Arduino already set them up.
    esp_netif_init();
    esp_event_loop_create_default();

    // RX stays at the IDF default. It was trimmed to 4 on the theory that "PSRAM
    // handles the rest" — it does not: WiFi static RX buffers are DMA-capable
    // internal DRAM and cannot live in PSRAM. All that trim bought was a receiver
    // four packets deep against a continuous ~220 pkt/s stream, i.e. unexplained
    // `lost` counts. TX is safe to trim because the send semaphore keeps exactly
    // one frame in flight.
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    wcfg.static_rx_buf_num  = WIFI_STATIC_RX_BUFFERS;
    wcfg.dynamic_tx_buf_num = WIFI_DYNAMIC_TX_BUFFERS;

    esp_err_t ret = esp_wifi_init(&wcfg);
    if (ret != ESP_OK) {
        LOG_ERROR("WiFi init failed: " + String(esp_err_to_name(ret)));
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_start();

    // Modem sleep parks the radio between beacons and makes ESP-NOW reception
    // miss packets. A continuous audio stream needs the receiver always on.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Fixed channel — must match on all nodes
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    LOG_INFO("Heap after WiFi init: " + String(ESP.getFreeHeap()) + " bytes");

    if (esp_now_init() != ESP_OK) {
        LOG_ERROR("ESP-NOW init failed");
        return;
    }

    esp_now_register_send_cb(onEspNowSent);
    esp_now_register_recv_cb(onEspNowRecv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        LOG_ERROR("ESP-NOW add peer failed");
        return;
    }

    txDone = xSemaphoreCreateBinary();
    if (!txDone) {
        LOG_ERROR("TX semaphore alloc failed");
        return;
    }
    xSemaphoreGive(txDone);   // radio starts idle

    txQueue = xQueueCreate(ESPNOW_TX_QUEUE_DEPTH, sizeof(AudioPacket));
    if (!txQueue) {
        LOG_ERROR("TX queue alloc failed");
        return;
    }
    xTaskCreatePinnedToCore(espnowTxTask, "espnow_tx", 4096, nullptr, 5, nullptr, 1);

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    LOG_INFO("ESP-NOW ready — MAC: " + String(macStr));
    LOG_INFO("Heap after ESP-NOW init: " + String(ESP.getFreeHeap()) + " bytes");
    espnowActive = true;
}

// ============================================================================
// Bluetooth A2DP — SERVER mode, BT Classic nodes only
// ============================================================================

#ifdef ENABLE_BLUETOOTH

BluetoothA2DPSink a2dpSink;
volatile bool btConnected       = false;
volatile bool doConnectSound    = false;
volatile bool doDisconnectSound = false;
// Not `btStarted` — Arduino's esp32-hal-bt.h already declares `bool btStarted()`
// at global scope, and the collision is a hard compile error.
static bool   btSinkStarted     = false;

// Half-band-ish 4-tap FIR [1 3 3 1]/8 running at the input rate. Taking every
// other sample without this folds all 11–22 kHz content back into the audible
// band (cymbals and sibilance turn to fizz), which is what naive decimation did.
static int32_t decimHist[4] = {0, 0, 0, 0};
static bool    decimPhase   = false;

static inline void decimReset() {
    decimHist[0] = decimHist[1] = decimHist[2] = decimHist[3] = 0;
    decimPhase = false;
}

// Called from BT task. Downsamples 44100Hz stereo → 22050Hz mono and queues
// ESP-NOW packets. The BT library still drives I2S locally (local playback).
static void a2dpDataCallback(const uint8_t *data, uint32_t length) {
    if (currentMode != MODE_SERVER || !txReady || !espnowActive) return;
    if (millis() - audioStartMs < TX_WARMUP_MS) return;

    const int16_t *in      = (const int16_t *)data;
    const int      nStereo = length / 4;   // 4 bytes per stereo sample pair

    for (int i = 0; i < nStereo; i++) {
        int32_t mono = ((int32_t)in[i * 2] + (int32_t)in[i * 2 + 1]) >> 1;

        decimHist[3] = decimHist[2];
        decimHist[2] = decimHist[1];
        decimHist[1] = decimHist[0];
        decimHist[0] = mono;

        decimPhase = !decimPhase;
        if (decimPhase) continue;   // output one sample per two inputs

        int32_t filtered =
            (decimHist[0] + 3 * decimHist[1] + 3 * decimHist[2] + decimHist[3]) >> 3;
        if (filtered >  32767) filtered =  32767;
        if (filtered < -32768) filtered = -32768;
        int16_t out = (int16_t)filtered;

        memcpy(accumBuf + accumLen, &out, 2);
        accumLen += 2;

        if (accumLen >= ESPNOW_PAYLOAD_SIZE) {
            AudioPacket pkt;
            pkt.seq = txSeq++;
            pkt.len = ESPNOW_PAYLOAD_SIZE;
            memcpy(pkt.data, accumBuf, ESPNOW_PAYLOAD_SIZE);
            if (xQueueSend(txQueue, &pkt, 0) != pdTRUE) txQueueFull++;
            accumLen = 0;
        }
    }
}

static void btConnectionChanged(esp_a2d_connection_state_t state, void *) {
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        LOG_INFO("BT connected");
        btConnected    = true;
        doConnectSound = true;
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        LOG_INFO("BT disconnected");
        btConnected       = false;
        doDisconnectSound = true;
    }
}

static void btAudioChanged(esp_a2d_audio_state_t state, void *) {
    if (state == ESP_A2D_AUDIO_STATE_STARTED) {
        txReady      = true;
        audioStartMs = millis();
        decimReset();
        accumLen = 0;
        LOG_INFO("BT audio started → ESP-NOW TX will activate in " + String(TX_WARMUP_MS) + "ms");
    } else {
        txReady      = false;
        audioStartMs = 0;
        LOG_INFO("BT audio stopped → ESP-NOW TX paused");
    }
}

static void startBluetooth() {
    if (btSinkStarted) return;

    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    a2dpSink.set_pin_config(pins);
    a2dpSink.set_stream_reader(a2dpDataCallback, true);  // true = keep local I2S output
    a2dpSink.set_on_connection_state_changed(btConnectionChanged);
    a2dpSink.set_on_audio_state_changed(btAudioChanged);
    a2dpSink.set_auto_reconnect(false);
    a2dpSink.set_volume(VOLUME_DEFAULT);
    a2dpSink.start(BT_DEVICE_NAME);
    btSinkStarted = true;
    LOG_INFO("BT discoverable as: " BT_DEVICE_NAME);
}

static void stopBluetooth() {
    if (!btSinkStarted) return;
    // end(false) keeps the BT controller in memory so start() works again.
    // end(true) releases it, and every later attempt to become a SERVER fails
    // until the node is power-cycled — which breaks the whole "any node can be
    // either role" premise.
    a2dpSink.end(false);
    btSinkStarted = false;
    LOG_INFO("BT stopped (controller retained for restart)");
}

#endif  // ENABLE_BLUETOOTH

// ============================================================================
// I2S Client Output — CLIENT mode
// ============================================================================

static bool clientI2SActive = false;

static void initI2SForClient() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = CLIENT_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = true
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        LOG_ERROR("I2S install (client) failed: " + String(esp_err_to_name(err)));
        return;
    }
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);
    clientI2SActive = true;
    LOG_INFO("I2S initialised for CLIENT mode at " + String(CLIENT_SAMPLE_RATE) + " Hz mono");
}

static void deinitI2SForClient() {
    if (!clientI2SActive) return;
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    clientI2SActive = false;
}

// Drive I2S from jitter buffer — called every loop() tick in CLIENT mode.
static void driveClientI2S() {
    if (!clientI2SActive) return;

    // Wait for prefill before starting output (prevents immediate underrun)
    if (!jReady) {
        if (jFill() >= JITTER_PREFILL) {
            jReady = true;
            LOG_INFO("Jitter buffer ready (" + String(jFill()) + " bytes) — starting I2S");
        } else {
            return;
        }
    }

    static int16_t mono[CLIENT_BATCH];
    static int16_t stereo[CLIENT_BATCH * 2];

    if (!jPeek((uint8_t *)mono, CLIENT_BATCH * 2)) {
        // Underrun. tx_desc_auto_clear already zeroes the DMA as it drains, so
        // pushing extra silence here would only add a click. Re-arm the prefill
        // gate and let the buffer refill. Counted, not logged — logging every
        // loop tick during an underrun makes the underrun worse.
        jReady = false;
        rxUnderrun++;
        return;
    }

    // Expand mono → stereo (duplicate sample to both channels)
    for (int i = 0; i < CLIENT_BATCH; i++) {
        stereo[i * 2]     = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }

    // Blocking with a short timeout paces this loop to the I2S sample clock.
    size_t bw = 0;
    i2s_write(I2S_NUM_0, stereo, CLIENT_BATCH * 4, &bw, pdMS_TO_TICKS(20));

    // Consume only what the DMA actually took. The previous version advanced
    // the read pointer unconditionally, discarding every sample I2S refused —
    // which at loop() speed was most of them.
    //
    // Round down to whole stereo frames first. i2s_write normally returns a
    // multiple of 4 here, but if it ever returned a partial frame, bw/2 would be
    // an odd number of mono bytes and the jitter buffer's 16-bit framing would be
    // permanently shifted — the exact failure jPushBlock exists to prevent.
    jAdvance((int)(bw & ~(size_t)3) / 2);   // 4 bytes written per 2 bytes of mono
}

// ============================================================================
// Mode Transitions
// ============================================================================

// Set when entering CLIENT mode failed, so the next attempt is held off.
static unsigned long clientRetryAfterMs = 0;

static void resetRxState() {
    jWrite = jRead = 0;
    jReady       = false;
    firstPkt     = true;
    rxActive     = false;
    senderLocked = false;
    lastRxMs     = 0;
    rxCount      = 0;
    rxDropped    = 0;
    rxOverflow   = 0;
    rxUnderrun   = 0;
    rxDupe       = 0;
    rxResync     = 0;
}

static void enterDiscovery() {
    LOG_INFO("=== DISCOVERY ===");

    txReady  = false;
    accumLen = 0;

    const bool wasClient = (currentMode == MODE_CLIENT);
    if (wasClient) deinitI2SForClient();

    // Reset on *every* entry to DISCOVERY, not just when leaving CLIENT.
    // senderLocked used to survive SERVER → DISCOVERY: a node that had once
    // heard server B would then silently ignore every broadcast from server C
    // and could never join the mesh again without a power cycle. rxActive
    // leaking the same way sent the node straight into CLIENT mode on a stream
    // that had stopped minutes earlier.
    resetRxState();

    if (wasClient) {
#ifdef ENABLE_BLUETOOTH
        delay(100);
        startBluetooth();
#endif
    }

    currentMode = MODE_DISCOVERY;
    LOG_INFO("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
}

static void enterServer() {
    LOG_INFO("=== SERVER — local playback + ESP-NOW broadcast ===");
    // BT is already running (started in setup). Just flip the mode;
    // the data callback will start forwarding once BT audio state goes STARTED.
#ifdef ENABLE_BLUETOOTH
    decimReset();
#endif
    accumLen    = 0;
    currentMode = MODE_SERVER;
}

static void enterClient() {
    LOG_INFO("=== CLIENT — ESP-NOW → I2S ===");

#ifdef ENABLE_BLUETOOTH
    LOG_INFO("Stopping BT to release I2S...");
    stopBluetooth();
    delay(200);
#endif

    jWrite = jRead = 0;
    jReady   = false;
    rxActive = false;
    firstPkt = true;

    initI2SForClient();
    if (!clientI2SActive) {
        LOG_ERROR("CLIENT I2S unavailable — returning to DISCOVERY");
#ifdef ENABLE_BLUETOOTH
        startBluetooth();
#endif
        // Back off before retrying. The server keeps broadcasting, so rxActive
        // is set again within milliseconds; without this the node would retry
        // every loop pass and thrash BT stop/start indefinitely.
        clientRetryAfterMs = millis() + CLIENT_RETRY_BACKOFF_MS;
        currentMode = MODE_DISCOVERY;
        return;
    }

    currentMode = MODE_CLIENT;
    LOG_INFO("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    DEBUG_SERIAL.begin(DEBUG_BAUD_RATE);
    delay(1000);

    DEBUG_SERIAL.println();
    DEBUG_SERIAL.println("================================");
    DEBUG_SERIAL.println("  SonoLoco — Multi-Room Audio");
#ifdef ENABLE_BLUETOOTH
    DEBUG_SERIAL.println("  Mode: SERVER capable (BT + ESP-NOW)");
#else
    DEBUG_SERIAL.println("  Mode: CLIENT only (ESP-NOW)");
#endif
    DEBUG_SERIAL.println("================================");
    DEBUG_SERIAL.println();

    LOG_INFO("Chip: "    + String(ESP.getChipModel()));
    LOG_INFO("CPU:  "    + String(ESP.getCpuFreqMHz()) + " MHz");
    LOG_INFO("Heap: "    + String(ESP.getFreeHeap()) + " bytes");
    LOG_INFO("PSRAM: "   + String(ESP.getPsramSize()) + " bytes");

    // Startup sound — only on nodes with a DAC connected (BT-capable nodes)
#ifdef ENABLE_BLUETOOTH
    initI2SForTones();
    playStartupSound();
    deinitI2SForTones();
#endif

    // ESP-NOW must be up before BT to ensure coexistence layer is ready
    setupESPNow();

#ifdef ENABLE_BLUETOOTH
    startBluetooth();
#endif

    LOG_INFO("Setup complete. Entering DISCOVERY...");
}

// ============================================================================
// Main Loop
// ============================================================================

void loop() {
    switch (currentMode) {

        case MODE_DISCOVERY:
#ifdef ENABLE_BLUETOOTH
            if (btConnected) {
                enterServer();
                break;
            }
#endif
            if (rxActive && espnowActive &&
                (long)(millis() - clientRetryAfterMs) >= 0) {
                enterClient();
            }
            break;

        case MODE_SERVER:
#ifdef ENABLE_BLUETOOTH
            if (doConnectSound) {
                doConnectSound = false;
                playConnectedSound();
            }
            if (!btConnected) {
                if (doDisconnectSound) {
                    doDisconnectSound = false;
                    playDisconnectedSound();
                }
                enterDiscovery();
            }
#endif
            break;

        case MODE_CLIENT:
            driveClientI2S();

            if (lastRxMs > 0 && millis() - lastRxMs > ESPNOW_SILENCE_TIMEOUT_MS) {
                LOG_INFO("ESP-NOW silent for " + String(ESPNOW_SILENCE_TIMEOUT_MS / 1000) + "s → DISCOVERY");
                LOG_INFO("RX stats: rx=" + String(rxCount) +
                         " lost=" + String(rxDropped) +
                         " overflow=" + String(rxOverflow) +
                         " underrun=" + String(rxUnderrun) +
                         " dupe=" + String(rxDupe) +
                         " resync=" + String(rxResync));
                enterDiscovery();   // clears the counters and lastRxMs
            }
            break;
    }

    // Periodic status (DEBUG_LEVEL >= 3 only)
#if DEBUG_LEVEL >= 3
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > STATUS_INTERVAL_MS) {
        lastStatus = millis();
        const char *modeStr =
            currentMode == MODE_DISCOVERY ? "DISCOVERY" :
            currentMode == MODE_SERVER    ? "SERVER"    : "CLIENT";

        if (currentMode == MODE_CLIENT) {
            LOG_INFO(String("Status: mode=") + modeStr +
                     " heap=" + String(ESP.getFreeHeap()) +
                     " jitter=" + String(jFill()) + "B" +
                     " rx=" + String(rxCount) +
                     " lost=" + String(rxDropped) +
                     " ovf=" + String(rxOverflow) +
                     " und=" + String(rxUnderrun) +
                     " dup=" + String(rxDupe) +
                     " rsy=" + String(rxResync));
        } else if (currentMode == MODE_SERVER) {
            LOG_INFO(String("Status: mode=") + modeStr +
                     " heap=" + String(ESP.getFreeHeap()) +
                     " qfull=" + String(txQueueFull) +
                     " senderr=" + String(txSendErr) +
                     " radiofail=" + String(txRadioFail));
        } else {
            LOG_INFO(String("Status: mode=") + modeStr +
                     " heap=" + String(ESP.getFreeHeap()));
        }
    }
#endif
}
