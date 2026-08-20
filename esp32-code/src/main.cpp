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
#include <esp_timer.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <esp_mac.h>   // esp_read_mac moved out of esp_system.h in IDF 5
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Hardware-independent parts of the CLIENT receive path, tested on the host
// with `pio test -e native` — see test/test_jitter.
#include "drift.h"
#include "jitter.h"
#include "seqtracker.h"

#ifdef ENABLE_BLUETOOTH
#include "BluetoothA2DPSink.h"
#endif

// ============================================================================
// State Machine
// ============================================================================

enum DeviceMode { MODE_DISCOVERY, MODE_SERVER, MODE_CLIENT };
volatile DeviceMode currentMode = MODE_DISCOVERY;

// ============================================================================
// Bench mode
// ============================================================================
//
// A runtime mode for automated multi-board testing — see tools/bench-mesh.ps1
// and docs/bench-test.md. It exists because the normal SERVER role needs a phone
// to connect over Bluetooth, which cannot be automated, so there would otherwise
// be no way to produce a stream unattended.
//
// In bench mode a node never starts Bluetooth. That matters for more than
// convenience: the BT/WiFi coexistence problem that keeps a WROOM off the mesh
// (D3) only exists while the BT stack is running, so with BT off a WROOM can
// join the mesh perfectly well. Bench mode therefore works on every board.
//
// The flag lives in RTC memory, which survives ESP.restart(). That is
// deliberate: bench mode has to be chosen before setup() decides whether to
// start Bluetooth, so the node has to be told and then restarted.
//
// RTC_NOINIT_ATTR, not RTC_DATA_ATTR. `.rtc.data` is re-initialised from the
// image on every boot that runs the bootloader, so a RTC_DATA_ATTR flag is
// already zero again by the time setup() reads it and the node just reboots
// into normal mode. `.rtc_noinit` is left alone, which is the whole point.
// The cost is that its value is undefined on a cold boot, hence a magic number
// rather than a bool.
#define BENCH_MAGIC 0xB0FFE501
RTC_NOINIT_ATTR static uint32_t benchMagic;

static bool benchMode   = false;   // this boot is a bench boot
static bool benchSource = false;   // this node is generating the test stream

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
    const int total = (BT_SAMPLE_RATE * durationMs) / 1000;
    const int chunk = 512;
    const int fade  = BT_SAMPLE_RATE * TONE_FADE_MS / 1000;
    int16_t buf[chunk * 2];
    int written = 0;

    while (written < total) {
        int n = min(chunk, total - written);

        for (int i = 0; i < n; i++) {
            float t = (float)(written + i) / BT_SAMPLE_RATE;
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
    const int total = (BT_SAMPLE_RATE * durationMs) / 1000;
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
        .sample_rate          = BT_SAMPLE_RATE,
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
// The ring buffer and the packet sequence accounting live in lib/jitter: they
// are pure logic, they are where the nastiest bugs in this project came from,
// and there they can be tested on this PC instead of only on a board. What
// stays here is the part that genuinely needs a radio and an I2S peripheral.
static_assert((JITTER_BUF_SIZE & (JITTER_BUF_SIZE - 1)) == 0,
              "JITTER_BUF_SIZE must be a power of two");
// An empty I2S DMA ring will accept CLIENT_DMA_CAPACITY_BYTES in one pass. If
// the prefill is smaller than that, the buffer is drained the instant it arms
// and underruns continuously — measured at 24/s on the first hardware run.
static_assert(JITTER_PREFILL > CLIENT_DMA_CAPACITY_BYTES,
              "JITTER_PREFILL must exceed what the I2S DMA ring can swallow at once");
static_assert(JITTER_PREFILL < JITTER_BUF_SIZE,
              "JITTER_PREFILL must fit in the jitter buffer");

static uint8_t      jStorage[JITTER_BUF_SIZE];
static JitterBuffer jbuf;
static SeqTracker   seqTracker(SEQ_RESYNC_THRESHOLD, MAX_GAP_FILL_PKTS);

// Clock-drift correction. The controller only decides; driveClientI2S applies.
// Runtime-switchable rather than compile-time so a bench run can measure the
// same boards with it on and off -- the before/after is the only evidence that
// it does anything, and D9's reasoning applies: test the binary that ships.
static DriftController driftCtl;
static bool            driftEnabled = true;

static volatile bool jReady   = false;   // true once prefill threshold is met
static volatile bool rxActive = false;   // set by recv callback, triggers mode switch
static volatile unsigned long lastRxMs = 0;
static volatile uint32_t rxCount    = 0;
static volatile uint32_t rxOverflow = 0;   // packets dropped, jitter buffer full
static volatile uint32_t rxUnderrun = 0;   // times playback outran the buffer
// lost / dupe / resync counters live on seqTracker.

// Lock onto the first node we hear so two simultaneous servers can never
// interleave their streams into one jitter buffer.
static uint8_t          lockedSender[6] = {0};
static volatile bool    senderLocked    = false;

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

    // Duplicate / reordered / restarted-server handling, including the unsigned
    // 65535-gap trap, is in SeqTracker and covered by the host tests.
    const SeqResult sr = seqTracker.update(seq);
    if (!sr.accept) return;   // exact retransmit — replaying it is an audible stutter

    // Substitute silence for lost packets so playback keeps its timing instead
    // of splicing the stream shorter on every loss.
    if (sr.fillPackets > 0 &&
        !jbuf.pushSilence(sr.fillPackets * ESPNOW_PAYLOAD_SIZE)) {
        rxOverflow++;
    }

    if (!jbuf.pushBlock(data + ESPNOW_HEADER_SIZE, n)) rxOverflow++;
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
    // Bench mode never starts Bluetooth, so there is no coexistence problem and
    // no reason to refuse WiFi — this is what lets a WROOM be tested at all.
    if (!benchMode && ESP.getPsramSize() == 0) {
        LOG_WARN("No PSRAM detected — WiFi/ESP-NOW disabled to protect BT heap.");
        LOG_WARN("This node will play locally only (no mesh). Upgrade to WROVER for full mesh.");
        return;
    }
    if (benchMode && ESP.getPsramSize() == 0) {
        LOG_INFO("Bench mode: no PSRAM, but Bluetooth is off, so ESP-NOW is safe here.");
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
        .dma_buf_count        = CLIENT_DMA_BUF_COUNT,
        .dma_buf_len          = CLIENT_DMA_BUF_LEN,
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
        if (jbuf.fill() >= JITTER_PREFILL) {
            jReady = true;
            LOG_INFO("Jitter buffer ready (" + String(jbuf.fill()) + " bytes) — starting I2S");
        } else {
            return;
        }
    }

    static int16_t mono[CLIENT_BATCH];
    static int16_t stereo[CLIENT_BATCH * 2];

    if (!jbuf.peek((uint8_t *)mono, CLIENT_BATCH * 2)) {
        // Underrun. tx_desc_auto_clear already zeroes the DMA as it drains, so
        // pushing extra silence here would only add a click. Re-arm the prefill
        // gate and let the buffer refill. Counted, not logged — logging every
        // loop tick during an underrun makes the underrun worse.
        jReady = false;
        rxUnderrun++;
        // The refill that follows is not drift. Integrating it would provoke a
        // burst of corrections on top of a buffer that is already unhappy.
        driftCtl.reset(millis());
        return;
    }

    // Ask before the batch, apply after it. Keeping the correction outside the
    // batch is what leaves the partial-write accounting below untouched: one
    // sample either side of a write whose length is already handled correctly,
    // rather than an edit in the middle of a buffer that I2S may only half take.
    DriftController::Correction corr = DriftController::NONE;
    if (driftEnabled) corr = driftCtl.update(millis(), jbuf.fill());

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
    // permanently shifted — the exact failure pushBlock exists to prevent.
    const size_t frames = (bw & ~(size_t)3) / 4;
    jbuf.advance((int)frames * 2);             // 4 bytes written per 2 bytes of mono

    // One sample of correction, at a batch boundary. At the drift these boards
    // actually have that is one edit every 1.5 s: inaudible, and it needs no
    // resampler. Nothing is counted unless it happened -- an I2S write that came
    // up short leaves the correction owed, and it is applied next batch instead.
    if (corr == DriftController::DROP) {
        // Consume a sample without playing it. Two bytes, so the buffer's 16-bit
        // framing survives -- the one thing this path must never get wrong.
        if (jbuf.fill() >= 2) {
            jbuf.advance(2);
            driftCtl.confirm(corr);
        }
    } else if (corr == DriftController::INSERT && frames > 0) {
        // Play a sample twice without consuming it: hold the value one extra
        // sample period. No discontinuity is possible by construction, because
        // it is the sample that was just played.
        const int16_t held  = mono[frames - 1];
        int16_t frame[2]    = {held, held};
        size_t  bwExtra     = 0;
        i2s_write(I2S_NUM_0, frame, sizeof(frame), &bwExtra, pdMS_TO_TICKS(20));
        if (bwExtra == sizeof(frame)) driftCtl.confirm(corr);
    }
}

// ============================================================================
// Mode Transitions
// ============================================================================

// Set when entering CLIENT mode failed, so the next attempt is held off.
static unsigned long clientRetryAfterMs = 0;

static void resetRxState() {
    jbuf.reset();
    seqTracker.reset();
    driftCtl.reset(millis());
    jReady       = false;
    rxActive     = false;
    senderLocked = false;
    lastRxMs     = 0;
    rxCount      = 0;
    rxOverflow   = 0;
    rxUnderrun   = 0;
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
        if (!benchMode) {   // bench mode never starts BT — see the bench section
            delay(100);
            startBluetooth();
        }
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
    if (!benchMode) {
        LOG_INFO("Stopping BT to release I2S...");
        stopBluetooth();
        delay(200);
    }
#endif

    jbuf.reset();
    seqTracker.reset();
    driftCtl.reset(millis());
    jReady   = false;
    rxActive = false;

    initI2SForClient();
    if (!clientI2SActive) {
        LOG_ERROR("CLIENT I2S unavailable — returning to DISCOVERY");
#ifdef ENABLE_BLUETOOTH
        if (!benchMode) startBluetooth();
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
// Bench mode — synthetic source, telemetry, serial control
// ============================================================================

static int64_t  benchStartUs      = 0;   // when this node started sourcing
static uint32_t benchPktIdx       = 0;   // packets that should have been sent by now
static uint32_t benchSampleIdx    = 0;   // running sample index, for a continuous tone
static uint32_t benchTxPackets    = 0;   // packets actually queued
static unsigned long benchLastReportMs = 0;

/**
 * Generate the test stream.
 *
 * Paced off `esp_timer_get_time()` rather than millis()/micros(): it is a 64-bit
 * microsecond counter, so it does not wrap at 71 minutes in the middle of a long
 * drift measurement. The due time for each packet is computed from the packet
 * index against the start time, not by adding an interval each pass, so rounding
 * cannot accumulate into exactly the drift we are trying to measure.
 */
static void benchServiceSource() {
    if (!benchSource || !espnowActive || txQueue == nullptr) return;

    const uint32_t samplesPerPkt = ESPNOW_PAYLOAD_SIZE / 2;
    const int64_t  now           = esp_timer_get_time();

    // Bounded catch-up: if this node was blocked for a while, send a few packets
    // back to back but never sit here spinning out a whole backlog.
    for (int burst = 0; burst < BENCH_MAX_CATCHUP_PKTS; burst++) {
        const int64_t due = benchStartUs +
            (int64_t)benchPktIdx * 1000000LL * (int64_t)samplesPerPkt / CLIENT_SAMPLE_RATE;
        if (now < due) return;

        AudioPacket pkt;
        pkt.seq = txSeq++;
        pkt.len = ESPNOW_PAYLOAD_SIZE;
        for (uint32_t i = 0; i < samplesPerPkt; i++) {
            const float t = (float)(benchSampleIdx + i) / CLIENT_SAMPLE_RATE;
            const int16_t s =
                (int16_t)(BENCH_TONE_AMPLITUDE * sinf(2.0f * M_PI * BENCH_TONE_HZ * t));
            memcpy(pkt.data + i * 2, &s, 2);
        }
        benchSampleIdx += samplesPerPkt;

        if (xQueueSend(txQueue, &pkt, 0) != pdTRUE) txQueueFull++;
        else                                        benchTxPackets++;
        benchPktIdx++;
    }
}

static void benchStartSource() {
    if (!espnowActive) {
        DEBUG_SERIAL.println("[BENCH] error reason=espnow_inactive");
        return;
    }
    benchStartUs   = esp_timer_get_time();
    benchPktIdx    = 0;
    benchSampleIdx = 0;
    benchTxPackets = 0;
    txQueueFull = txSendErr = txRadioFail = 0;
    benchSource    = true;
    currentMode    = MODE_SERVER;   // stops this node treating its own role as a listener
    DEBUG_SERIAL.printf("[BENCH] source start rate=%d hz=%d amp=%d\n",
                        CLIENT_SAMPLE_RATE, BENCH_TONE_HZ, BENCH_TONE_AMPLITUDE);
}

static void benchStopSource() {
    benchSource = false;
    DEBUG_SERIAL.printf("[BENCH] source stop tx=%lu\n", (unsigned long)benchTxPackets);
    enterDiscovery();
}

/**
 * One machine-parsable line per interval, in every mode, with every field
 * present whether or not it applies. `ms` is this node's own clock: the host
 * regresses it against PC time to get each board's ppm error, and the difference
 * between two boards is their relative drift.
 *
 * printf, not String concatenation — this runs once a second forever and the
 * String version fragments the heap.
 */
static void benchReport() {
    const char *modeStr = currentMode == MODE_DISCOVERY ? "DISCOVERY"
                        : currentMode == MODE_SERVER    ? "SERVER"
                                                        : "CLIENT";
    DEBUG_SERIAL.printf(
        "[BENCH] ms=%lu role=%s mode=%s heap=%lu jit=%d rx=%lu lost=%lu ovf=%lu "
        "und=%lu dup=%lu rsy=%lu tx=%lu qfull=%lu senderr=%lu radiofail=%lu "
        "drift=%d ins=%lu drp=%lu dr=%.3f srx=%ld\n",
        (unsigned long)millis(),
        benchSource ? "SOURCE" : "SINK",
        modeStr,
        (unsigned long)ESP.getFreeHeap(),
        jbuf.fill(),
        (unsigned long)rxCount,
        (unsigned long)seqTracker.lost,
        (unsigned long)rxOverflow,
        (unsigned long)rxUnderrun,
        (unsigned long)seqTracker.dupe,
        (unsigned long)seqTracker.resync,
        (unsigned long)benchTxPackets,
        (unsigned long)txQueueFull,
        (unsigned long)txSendErr,
        (unsigned long)txRadioFail,
        driftEnabled ? 1 : 0,
        (unsigned long)driftCtl.inserted,
        (unsigned long)driftCtl.dropped,
        // Corrections per second the controller is currently asking for. Once
        // the loop is closed this is the only in-band measure of drift left:
        // a corrected buffer no longer has a slope to regress.
        driftCtl.rate(),
        // Age of the last received packet, as the silence check sees it. Signed
        // and printed even when it is meaningless (a source has no rx), because
        // a value that goes *negative* or jumps is the evidence that the check
        // is misfiring rather than the radio going quiet.
        lastRxMs ? (long)(millis() - lastRxMs) : -1L);
}

static void benchIdentify() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    DEBUG_SERIAL.printf(
        "[BENCH] id fw=%s chip=%s psram=%lu mac=%02X:%02X:%02X:%02X:%02X:%02X "
        "bench=%d bt=%d espnow=%d drift=%d name=%s\n",
        FW_VERSION,
        ESP.getChipModel(),
        (unsigned long)ESP.getPsramSize(),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        benchMode ? 1 : 0,
#ifdef ENABLE_BLUETOOTH
        1,
#else
        0,
#endif
        espnowActive ? 1 : 0,
        driftEnabled ? 1 : 0,
        ROOM_NAME);
}

/**
 * Single-character commands, so the host side needs no protocol.
 *
 *   ?  identify          b  reboot into bench mode     n  reboot into normal mode
 *   s  start sourcing    x  stop sourcing              r  report now
 *   d  toggle clock-drift correction
 */
static void benchServiceSerial() {
    while (DEBUG_SERIAL.available()) {
        switch (DEBUG_SERIAL.read()) {
            case '?': benchIdentify(); break;
            case 'r': benchReport();   break;
            case 'd':
                // No restart needed: the controller is re-armed rather than
                // reconfigured, so one run can measure the same boards with
                // correction off and on without reflashing between them.
                driftEnabled = !driftEnabled;
                driftCtl.reset(millis());
                DEBUG_SERIAL.printf("[BENCH] drift=%d\n", driftEnabled ? 1 : 0);
                break;
            case 's': benchStartSource(); break;
            case 'x': benchStopSource();  break;
            case 'b':
                benchMagic = BENCH_MAGIC;
                DEBUG_SERIAL.println("[BENCH] rebooting into bench mode");
                DEBUG_SERIAL.flush();
                delay(50);
                ESP.restart();
                break;
            case 'n':
                benchMagic = 0;
                DEBUG_SERIAL.println("[BENCH] rebooting into normal mode");
                DEBUG_SERIAL.flush();
                delay(50);
                ESP.restart();
                break;
            default: break;   // ignore newlines and anything unrecognised
        }
    }
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
    DEBUG_SERIAL.println("  Firmware: " FW_VERSION);
#ifdef ENABLE_BLUETOOTH
    DEBUG_SERIAL.println("  Mode: SERVER capable (BT + ESP-NOW)");
#else
    DEBUG_SERIAL.println("  Mode: CLIENT only (ESP-NOW)");
#endif
    DEBUG_SERIAL.println("================================");
    DEBUG_SERIAL.println();

    benchMode = (benchMagic == BENCH_MAGIC);
    if (benchMode) {
        DEBUG_SERIAL.println("[BENCH] boot bench=1 (Bluetooth disabled this boot)");
    }

    LOG_INFO("Chip: "    + String(ESP.getChipModel()));
    LOG_INFO("CPU:  "    + String(ESP.getCpuFreqMHz()) + " MHz");
    LOG_INFO("Heap: "    + String(ESP.getFreeHeap()) + " bytes");
    LOG_INFO("PSRAM: "   + String(ESP.getPsramSize()) + " bytes");

    // Startup sound — only on nodes with a DAC connected (BT-capable nodes).
    // Skipped in bench mode: it would delay the first telemetry line and it is
    // played through an I2S driver that the client path is about to reconfigure.
#ifdef ENABLE_BLUETOOTH
    if (!benchMode) {
        initI2SForTones();
        playStartupSound();
        deinitI2SForTones();
    }
#endif

    DriftController::Config dcfg;
    dcfg.targetBytes   = DRIFT_TARGET_BYTES;
    dcfg.deadbandBytes = DRIFT_DEADBAND_BYTES;
    dcfg.kp            = DRIFT_KP;
    dcfg.maxRatePerSec = DRIFT_MAX_RATE;
    dcfg.emaTauMs      = DRIFT_EMA_TAU_MS;
    dcfg.settleMs      = DRIFT_SETTLE_MS;
    // begin() here and reset() everywhere else: the correction counters have to
    // outlive a mode change, or a bench run cannot total them.
    driftCtl.begin(dcfg, millis());

    // Must happen before ESP-NOW comes up: the recv callback starts pushing into
    // this buffer as soon as the radio is listening.
    if (!jbuf.init(jStorage, JITTER_BUF_SIZE)) {
        LOG_ERROR("Jitter buffer init failed — JITTER_BUF_SIZE must be a power of two");
    }

    // ESP-NOW must be up before BT to ensure coexistence layer is ready
    setupESPNow();

#ifdef ENABLE_BLUETOOTH
    if (!benchMode) startBluetooth();
#endif

    LOG_INFO("Setup complete. Entering DISCOVERY...");
    if (benchMode) benchIdentify();
}

// ============================================================================
// Main Loop
// ============================================================================

void loop() {
    // Always listening for host commands, in every mode — this is how a node is
    // put into bench mode in the first place.
    benchServiceSerial();
    benchServiceSource();

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
            // In bench mode SERVER means "sourcing the synthetic stream" and
            // there is no BT connection to lose, so none of this applies —
            // without the guard the node would leave SERVER on the first pass.
            if (!benchMode) {
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
            }
#endif
            break;

        case MODE_CLIENT:
            driveClientI2S();

            // Read both once. Recomputing millis() or re-reading lastRxMs for
            // the log would report different numbers than the ones that made the
            // decision, which is exactly what you do not want when the decision
            // itself is under suspicion.
            {
            const unsigned long nowMs  = millis();
            const unsigned long lastMs = lastRxMs;
            if (lastMs > 0 && nowMs - lastMs > ESPNOW_SILENCE_TIMEOUT_MS) {
                DEBUG_SERIAL.printf("[BENCH] silence now=%lu last=%lu delta=%lu rx=%lu\n",
                                    nowMs, lastMs, nowMs - lastMs, (unsigned long)rxCount);
                LOG_INFO("ESP-NOW silent for " + String(ESPNOW_SILENCE_TIMEOUT_MS / 1000) + "s → DISCOVERY");
                LOG_INFO("RX stats: rx=" + String(rxCount) +
                         " lost=" + String(seqTracker.lost) +
                         " overflow=" + String(rxOverflow) +
                         " underrun=" + String(rxUnderrun) +
                         " dupe=" + String(seqTracker.dupe) +
                         " resync=" + String(seqTracker.resync));
                enterDiscovery();   // clears the counters and lastRxMs
            }
            }
            break;
    }

    // Bench telemetry replaces the human-readable status line: the host parses
    // it, and two status lines a second would just interleave confusingly.
    if (benchMode) {
        if (millis() - benchLastReportMs >= BENCH_REPORT_MS) {
            benchLastReportMs = millis();
            benchReport();
        }
        return;
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
                     " jitter=" + String(jbuf.fill()) + "B" +
                     " rx=" + String(rxCount) +
                     " lost=" + String(seqTracker.lost) +
                     " ovf=" + String(rxOverflow) +
                     " und=" + String(rxUnderrun) +
                     " dup=" + String(seqTracker.dupe) +
                     " rsy=" + String(seqTracker.resync));
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
