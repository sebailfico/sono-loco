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
#include <Preferences.h>

// Hardware-independent parts of the CLIENT receive path, tested on the host
// with `pio test -e native` — see test/test_jitter.
#include "drift.h"
#include "jitter.h"
#include "mesh.h"
#include "seqtracker.h"

#ifdef ENABLE_BLUETOOTH
#include "BluetoothA2DPSink.h"
#include <esp_bt.h>
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
// Client-only mode
// ============================================================================
//
// A node told never to be a server: Bluetooth is not started, so ESP-NOW runs
// without the coexistence problem that needs PSRAM, and a WROOM becomes a usable
// mesh client instead of a standalone speaker. See the D3 amendment.
//
// Unlike bench mode this is a *setting*, not a test mode, so it lives in NVS
// rather than RTC memory. RTC memory survives a restart but not a power cut, and
// a node wired into a room is expected to come back as what it was after the
// power blinks. Bench mode staying volatile is equally deliberate: a board left
// in a test mode by a power cut is a trap, not a feature.
static Preferences prefs;
static bool        clientOnly = false;

static const char *PREF_NAMESPACE = "sonoloco";
static const char *PREF_CLIENT_ONLY = "clientonly";
static const char *PREF_MESH_ID = "meshid";
static const char *PREF_MESH_NAME = "meshname";

/**
 * Whether this boot may run the Bluetooth stack at all.
 *
 * One predicate rather than a condition repeated at each call site: BT is
 * started from setup() and restarted whenever a node leaves CLIENT, and a node
 * that skipped BT at boot must not acquire it later on a mode change. On a WROOM
 * that would be the D3 crash arriving several minutes after boot, which is a
 * miserable thing to debug.
 */
static inline bool btAllowed() {
    return !benchMode && !clientOnly;
}

// ============================================================================
// Mesh identity
// ============================================================================
//
// Which household a node belongs to. Every packet carries the 16-bit id and a
// client drops anything that is not its own, so two SonoLoco installations in
// radio range stop joining each other's music -- see D12 and lib/mesh/, which
// holds the name-to-id derivation and its host tests.
//
// In NVS rather than in the build, and for a stronger reason than client-only
// mode: this is the one setting that has to be changeable on a board somebody
// already owns and has already screwed to a wall. Two nodes are in the same
// mesh if and only if this number matches, so it also has to survive a power
// cut -- a node that came back on the factory default would silently rejoin
// whichever neighbour is still on it.
//
// The *name* is stored alongside the id purely so the logs can say "casa rossi"
// instead of "6F59". It is empty on a node that was paired rather than named,
// because only the id travels on the wire and there is nothing to invert it to.
static uint16_t meshId = MESH_ID_UNSET;
static char     meshName[MESH_NAME_MAX + 1] = {0};

// Pairing: adopt the mesh of the next foreign stream heard. `pairCandidate` is
// written from the ESP-NOW receive callback and committed in loop(), for the
// same reason the rest of that callback only ever touches counters -- a flash
// write there stalls the radio.
static volatile bool     pairing            = false;
static unsigned long     pairingUntilMs     = 0;
static volatile uint16_t pairCandidate      = MESH_ID_UNSET;
static volatile bool     pairCandidateReady = false;

// The other half of the handshake: this node is announcing its own mesh so a
// node being moved can adopt it. Both windows have to be open at once, which is
// what makes pairing require a person at each end.
static bool          offering        = false;
static unsigned long offeringUntilMs = 0;
static unsigned long lastBeaconMs    = 0;

// Packets dropped because they belong to somebody else's mesh. Worth counting
// rather than ignoring: it is the difference between "the neighbours are
// audible but correctly ignored" and "nothing is arriving at all", which look
// identical from every other number this firmware reports.
static volatile uint32_t rxForeign = 0;

/** Read the configured mesh identity out of NVS. Called once, before the radio. */
static void meshLoadIdentity() {
    prefs.begin(PREF_NAMESPACE, true);
    prefs.getString(PREF_MESH_NAME, meshName, sizeof(meshName));
    const uint16_t storedId = prefs.getUShort(PREF_MESH_ID, MESH_ID_UNSET);
    prefs.end();

    // A stored name wins over a stored id, because the name is what the id was
    // derived from. They only ever disagree if the hash itself changed, and in
    // that case every node re-deriving from its name still agrees with every
    // other -- which is the outcome to prefer over half the house keeping an id
    // nobody can reproduce.
    meshId = meshIdFromName(meshName);
    if (meshId == MESH_ID_UNSET) meshId = storedId;   // paired, not named
    if (meshId == MESH_ID_UNSET) {                    // never configured at all
        meshNormaliseName(MESH_NAME, meshName, sizeof(meshName));
        meshId = meshIdFromName(meshName);
    }
}

/** The name for logs. Never empty, so a format string cannot print a bare gap. */
static const char *meshNameForLog() {
    return meshName[0] ? meshName : "(paired)";
}

/** Adopt a name: store it, store the id it derives, and start using both. */
static bool meshSetName(const char *name) {
    char norm[MESH_NAME_MAX + 1];
    meshNormaliseName(name, norm, sizeof(norm));
    const uint16_t id = meshIdFromName(norm);
    if (id == MESH_ID_UNSET) return false;   // empty or whitespace only

    prefs.begin(PREF_NAMESPACE, false);
    prefs.putString(PREF_MESH_NAME, norm);
    prefs.putUShort(PREF_MESH_ID, id);
    prefs.end();

    strncpy(meshName, norm, sizeof(meshName) - 1);
    meshName[sizeof(meshName) - 1] = '\0';
    meshId = id;
    return true;
}

/** Adopt an id heard on the air. The name is cleared: it cannot be recovered. */
static void meshAdoptId(uint16_t id) {
    prefs.begin(PREF_NAMESPACE, false);
    prefs.remove(PREF_MESH_NAME);
    prefs.putUShort(PREF_MESH_ID, id);
    prefs.end();

    meshName[0] = '\0';
    meshId      = id;
}

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

#define ESPNOW_HEADER_SIZE 6   // group (2) + seq (2) + len (2)

// `group` is first because it is the field that decides whether the rest is
// even ours to look at -- a receiver rejects a neighbour's packet having read
// two bytes.
typedef struct __attribute__((packed)) {
    uint16_t group;
    uint16_t seq;
    uint16_t len;
    uint8_t  data[ESPNOW_PAYLOAD_SIZE];
} AudioPacket;  // 206 bytes total, well under the 250-byte ESP-NOW limit

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

// The ring's storage. A static array would sit in internal DRAM on every
// board; on a board with PSRAM it goes there instead, because the server
// path needs the internal bytes for the BT stack and the ring is only ever
// touched from task context (the ESP-NOW receive callback and loop()), where
// PSRAM is fine. On a WROOM/S3/C3 it stays static.
static uint8_t      jStorageInternal[JITTER_BUF_SIZE];
static uint8_t     *jStorage = jStorageInternal;
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

    // The received buffer has no alignment guarantee; copy the header out
    // instead of casting to a struct pointer.
    uint16_t group, seq, plen;
    memcpy(&group, data,     2);
    memcpy(&seq,   data + 2, 2);
    memcpy(&plen,  data + 4, 2);

    const bool beacon = meshIsBeacon(seq, plen);

    // Whose mesh is this? Checked BEFORE the sender lock below, and the order is
    // the whole point: a neighbour's server that took the lock would leave this
    // node ignoring its own household until it next fell back to DISCOVERY.
    if (group != meshId) {
        // Adoption happens only from a beacon -- somebody is holding the button
        // on a node of that mesh right now. Adopting from any foreign *stream*,
        // as this first did, let a neighbour capture a node by doing nothing
        // more deliberate than playing music inside the pairing window.
        //
        // Only the intent is recorded here: the NVS write happens in loop(),
        // because this is the WiFi task and a flash erase here would stall the
        // radio mid-stream.
        if (beacon && pairing && !pairCandidateReady) {
            pairCandidate      = group;
            pairCandidateReady = true;
        }
        rxForeign++;
        return;
    }

    // Our own mesh offering itself to somebody else. Nothing to do, and it must
    // not fall through: MESH_BEACON_SEQ arriving at the sequence tracker looks
    // like a stream restart and would charge a resync and re-arm the buffer.
    if (beacon) return;

    if (!senderLocked) {
        memcpy(lockedSender, srcMac, 6);
        senderLocked = true;
    } else if (memcmp(lockedSender, srcMac, 6) != 0) {
        return;   // a second server is broadcasting — ignore it
    }

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
    // Bench mode never starts Bluetooth, so there is no coexistence problem and
    // no reason to refuse WiFi — this is what lets a WROOM be tested at all.
    // The guard is about BT and WiFi running *together*. Bench mode and
    // client-only mode both mean Bluetooth is never started, so neither needs
    // PSRAM to run the mesh — which is exactly what makes a WROOM a usable
    // client rather than a standalone speaker. See the D3 amendment.
#ifdef ENABLE_BLUETOOTH
    const bool btWillStart = btAllowed();
    if (btWillStart && ESP.getPsramSize() == 0) {
        LOG_WARN("No PSRAM detected — WiFi/ESP-NOW disabled to protect BT heap.");
        LOG_WARN("This node will play locally only (no mesh). Set client-only mode ('c')");
        LOG_WARN("to use it as a mesh client instead, or upgrade to a WROVER.");
        return;
    }
    if (!btWillStart && ESP.getPsramSize() == 0) {
        LOG_INFO("No PSRAM, but Bluetooth is off this boot, so ESP-NOW is safe here.");
    }
#else
    const bool btWillStart = false;   // no BT Classic on this chip
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
    wcfg.static_tx_buf_num  = WIFI_STATIC_TX_BUFFERS;
    // ESP-NOW frames are single management frames: no aggregation, no block
    // ack, and nobody here reads channel state information. Each of these
    // costs internal DRAM at init on a node that is about to need every byte
    // of it for the BT stack.
    wcfg.ampdu_rx_enable = 0;
    wcfg.ampdu_tx_enable = 0;
    wcfg.csi_enable      = 0;

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
    //
    // But NOT on a node that runs Bluetooth. The IDF coexistence layer requires
    // WiFi modem sleep while the BT controller is enabled, and it enforces that
    // with abort(), not an error code. Measured on the first WROVER (2026-09-14):
    // PS_NONE set here, then BT started  -> abort() in coex_core_enable, boot loop;
    // BT started, then PS_NONE set       -> abort() in pm_set_sleep_type, boot loop.
    // So it is not an ordering question -- a BT node keeps the IDF default
    // (WIFI_PS_MIN_MODEM). Whether that actually costs a BT node any ESP-NOW
    // packets is untested: modem sleep is documented to engage only while
    // associated with an AP, which this mesh never is. See TODO.
    if (!btWillStart) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }

    // Fixed channel — must match on all nodes
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    // After esp_wifi_start(), per the IDF header. See ESPNOW_PHY_RATE.
    ret = esp_wifi_config_espnow_rate(WIFI_IF_STA, ESPNOW_PHY_RATE);
    if (ret != ESP_OK) {
        LOG_WARN("ESP-NOW rate config failed: " + String(esp_err_to_name(ret)) + " — staying at the 1 Mbps default");
    }
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
    // Signed, for the reason spelled out at the ESP-NOW silence check: this
    // timestamp is written from the A2DP state callback, and an unsigned
    // difference against a timestamp set a moment in the future wraps to a huge
    // number — here that would silently skip the warmup instead of enforcing it.
    if ((long)(millis() - audioStartMs) < (long)TX_WARMUP_MS) return;

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
            pkt.group = meshId;
            pkt.seq   = txSeq++;
            pkt.len   = ESPNOW_PAYLOAD_SIZE;
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

/**
 * Bring the BT controller up in Classic-only mode, before the A2DP library
 * gets to it.
 *
 * Left to itself the library calls the Arduino core's btStart(), which
 * initialises the controller in dual mode (BLE + Classic) because that is what
 * the core's sdkconfig selects. The BLE half then holds internal DRAM for
 * three BLE connections this firmware never makes -- and internal DRAM is the
 * one thing a WROVER server is short of: with WiFi and BT up it had 15 KB left,
 * and the first phone to connect took it to zero (assert in Bluedroid's
 * hash_map_set, 2026-09-14). Releasing the BLE memory first and initialising
 * Classic-only is what the library itself does on non-Arduino builds.
 *
 * The library's start() finds the controller already ENABLED and leaves it
 * alone; its end(false) never touches the controller, so a CLIENT -> SERVER
 * restart finds it still up. BLE is gone for good after this -- a provisioning
 * scheme over BLE (TODO.md) would have to give some of this memory back.
 */
static void bringUpBtController() {
    esp_bt_controller_status_t st = esp_bt_controller_get_status();
    if (st == ESP_BT_CONTROLLER_STATUS_ENABLED) return;
    if (st == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);   // no-op if already done
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        cfg.mode = ESP_BT_MODE_CLASSIC_BT;
        esp_err_t r = esp_bt_controller_init(&cfg);
        if (r != ESP_OK) {
            LOG_ERROR("BT controller init failed: " + String(esp_err_to_name(r)));
            return;
        }
        while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) delay(10);
    }
    esp_err_t r = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (r != ESP_OK) LOG_ERROR("BT controller enable failed: " + String(esp_err_to_name(r)));
}

static void startBluetooth() {
    if (btSinkStarted) return;

    bringUpBtController();

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
    LOG_INFO("Heap after BT start: " + String(ESP.getFreeHeap()) + " bytes, maxalloc " +
             String(ESP.getMaxAllocHeap()));
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

    // One sample of correction, at a batch boundary -- an edit every second or
    // two at the offsets these boards actually have. Inaudible, and it needs no
    // resampler. Nothing is counted unless it happened: an I2S write that came up
    // short leaves the correction owed, and it is applied next batch instead.
    //
    // Both directions require frames > 0, i.e. that playback actually advanced.
    // A correction is a statement about the rate audio is being consumed at, and
    // if the DMA took nothing then it was not consumed at any rate; dropping a
    // sample there would discard audio that was never played, to fix a drift that
    // did not accrue.
    if (corr == DriftController::DROP && frames > 0) {
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
    rxForeign    = 0;
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
        if (btAllowed()) {
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
    if (btAllowed()) {
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
        if (btAllowed()) startBluetooth();
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
// Pairing — joining a mesh with a button press at each end
// ============================================================================
//
// Naming a mesh needs a serial console, which a node screwed to a wall does not
// have. Pairing is the alternative, and it takes a press at both ends: hold the
// button on a node of the mesh being joined and it *offers* itself for a
// minute, hold the button on the node being moved and it *listens*, adopting
// the first offer it hears.
//
// Two presses on purpose. Adopting from any foreign stream -- which is what the
// first version did -- let a neighbour capture a node by doing nothing more
// deliberate than playing music during the window. An offer has to be made by
// somebody standing at the other mesh, pressing its button, in the same minute.
//
// Which half a press performs follows the node's role, so there is nothing for
// the user to choose: a node that can be a server offers the mesh, a speaker
// joins one. Both halves are also available over serial ('o' and 'p'), which is
// how the cases the button cannot express are reached -- moving a server into
// somebody else's mesh, most obviously.
//
// Adoption copies an id off the air, so unlike a typed name it cannot land in
// the wrong mesh through a hash collision. What it cannot recover is the name:
// only the id travels, so a paired node logs "(paired)" from then on.

enum MeshFeedback { MESH_FB_OPEN, MESH_FB_PAIRED, MESH_FB_CLOSED };

/**
 * Say what just happened, on the speaker the node already has.
 *
 * This is the only acknowledgement a user without a serial console ever gets,
 * and pairing without it is a button that appears to do nothing.
 *
 * Only in DISCOVERY. In CLIENT mode the I2S driver is configured for the mesh
 * stream and the jitter buffer is feeding it; on a SERVER the A2DP task owns
 * it, and writing tones into a driver another task is driving is the conflict
 * `TODO.md` already has an open item about. Both are also cases where the user
 * can hear perfectly well that the node is alive.
 */
static void meshPlayFeedback(MeshFeedback what) {
    if (currentMode != MODE_DISCOVERY) return;

    // Whoever already owns the driver keeps it. On a BT-capable node sitting in
    // DISCOVERY that is the A2DP sink, which is exactly what the connect and
    // disconnect tones are written into; installing a second driver over it
    // fails, and the tone would be lost.
#ifdef ENABLE_BLUETOOTH
    const bool someoneElseOwnsI2S = btSinkStarted || clientI2SActive;
#else
    const bool someoneElseOwnsI2S = clientI2SActive;
#endif
    if (!someoneElseOwnsI2S) initI2SForTones();

    switch (what) {
        case MESH_FB_OPEN:   playStartupSound();      break;   // two beeps: I am listening
        case MESH_FB_PAIRED: playConnectedSound();    break;   // rising: joined
        case MESH_FB_CLOSED: playDisconnectedSound(); break;   // falling: nothing heard
    }

    if (!someoneElseOwnsI2S) deinitI2SForTones();
}

/** Listen for an offer, and adopt the first one heard. */
static void meshStartPairing() {
    // Stop playing first. This node is about to belong to a different mesh, and
    // the press has to be audible: music stopping and then two beeps is what
    // tells somebody with no console that the button registered.
    if (currentMode == MODE_CLIENT) enterDiscovery();

    pairCandidate      = MESH_ID_UNSET;
    pairCandidateReady = false;
    pairingUntilMs     = millis() + MESH_PAIR_WINDOW_MS;
    pairing            = true;
    DEBUG_SERIAL.printf("[MESH] listening %lus for an offer (current mesh=%04X %s)\n",
                        (unsigned long)(MESH_PAIR_WINDOW_MS / 1000),
                        meshId, meshNameForLog());
    meshPlayFeedback(MESH_FB_OPEN);
}

/** Announce this node's mesh, so a node being moved can adopt it. */
static void meshStartOffering() {
    offeringUntilMs = millis() + MESH_PAIR_WINDOW_MS;
    lastBeaconMs    = 0;          // first beacon goes out on the next loop pass
    offering        = true;
    DEBUG_SERIAL.printf("[MESH] offering mesh=%04X %s for %lus\n",
                        meshId, meshNameForLog(),
                        (unsigned long)(MESH_PAIR_WINDOW_MS / 1000));
    meshPlayFeedback(MESH_FB_OPEN);
}

/**
 * Send the beacons while an offer is open. Called from loop() in every mode.
 *
 * A beacon is an audio packet with no payload (see `meshIsBeacon`), so it goes
 * out through the same queue and the same send gate as everything else -- there
 * is no second transmit path to keep correct. At 5/s against a stream's 220/s
 * it costs nothing measurable, which is what lets a server offer while it is
 * still playing.
 */
static void meshServiceOffer() {
    if (!offering) return;

    if ((long)(millis() - offeringUntilMs) >= 0) {
        offering = false;
        DEBUG_SERIAL.printf("[MESH] offer closed mesh=%04X\n", meshId);
        // No tone: an offer closing says nothing about whether anybody joined,
        // and the node that made it is quite likely mid-stream.
        return;
    }

    if (!espnowActive || txQueue == nullptr) return;
    if (lastBeaconMs != 0 &&
        (long)(millis() - lastBeaconMs) < (long)MESH_BEACON_INTERVAL_MS) return;
    lastBeaconMs = millis();

    AudioPacket pkt;
    pkt.group = meshId;
    pkt.seq   = MESH_BEACON_SEQ;
    pkt.len   = 0;                 // the mesh id in the header is the whole message
    if (xQueueSend(txQueue, &pkt, 0) != pdTRUE) txQueueFull++;
}

/**
 * Commit or expire a listening window. Called from loop() in every mode.
 *
 * An adopted node drops straight back to DISCOVERY: it may be holding a locked
 * sender and a half-full jitter buffer from the mesh it just left, and none of
 * that means anything any more. A bench source is exempted because it is
 * transmitting, not listening, and DISCOVERY would be a mode change in the
 * middle of a measurement.
 */
static void meshServicePairing() {
    if (!pairing) return;

    if (pairCandidateReady) {
        const uint16_t id = pairCandidate;
        pairing            = false;
        pairCandidateReady = false;
        if (id != MESH_ID_UNSET) {
            meshAdoptId(id);
            DEBUG_SERIAL.printf("[MESH] paired mesh=%04X\n", meshId);
            if (!benchSource) enterDiscovery();
            meshPlayFeedback(MESH_FB_PAIRED);
        }
        return;
    }

    // Signed, like every other deadline compared against millis() here.
    if ((long)(millis() - pairingUntilMs) >= 0) {
        pairing = false;
        DEBUG_SERIAL.printf("[MESH] listening closed, no offer heard (mesh=%04X %s)\n",
                            meshId, meshNameForLog());
        meshPlayFeedback(MESH_FB_CLOSED);
    }
}

/**
 * The long press.
 *
 * A press *while running*, never a press held through a reset: the BOOT button
 * these boards use is a strapping pin, and holding it across a reset puts the
 * chip into the ROM download mode, where no firmware runs at all.
 *
 * No debounce beyond the hold itself. A bounce is milliseconds and interrupts
 * the press, which restarts a timer that has to reach three seconds.
 */
static void meshServiceButton() {
#if MESH_PAIR_BUTTON_PIN >= 0
    static bool          held           = false;
    static unsigned long heldSinceMs    = 0;
    static bool          firedThisPress = false;

    const bool down = (digitalRead(MESH_PAIR_BUTTON_PIN) == LOW);

    if (!down) {
        held           = false;
        firedThisPress = false;
        return;
    }

    if (!held) {
        held        = true;
        heldSinceMs = millis();
        return;
    }

    if (!firedThisPress &&
        (long)(millis() - heldSinceMs) >= (long)MESH_PAIR_HOLD_MS) {
        firedThisPress = true;   // one window per press, not one per loop pass
        // Role decides which half of the handshake a press is. A node that can
        // be a server holds the mesh others join; a speaker is what gets moved.
        // Nothing for the user to pick, and it matches where the two boxes
        // physically are.
        if (btAllowed()) meshStartOffering();
        else             meshStartPairing();
    }
#endif
}

// ============================================================================
// Bench mode — synthetic source, telemetry, serial control
// ============================================================================

static int64_t  benchStartUs      = 0;   // when this node started sourcing
static uint32_t benchPktIdx       = 0;   // packets that should have been sent by now
static uint32_t benchSampleIdx    = 0;   // running sample index, for a continuous tone

/**
 * The test tone, precomputed.
 *
 * Generating it with sinf() per sample put a hard ceiling on how fast a node
 * could source: an ESP32-C3 managed 37.8 packets/s against the 220.5 the stream
 * needs, and starved its client into 159 underruns in 90 s. The C3 has no FPU at
 * all, and `2.0f * M_PI * BENCH_TONE_HZ * t` is worse than it looks — M_PI is a
 * *double*, so the whole expression is evaluated in soft-float double before
 * being handed to sinf. The radio was never the bottleneck: qfull and senderr
 * were both zero throughout.
 *
 * The table holds an exact whole number of tone periods, so playing it end to
 * end and wrapping is seamless — no phase discontinuity, no click. That length
 * is sampleRate / gcd(sampleRate, toneHz): 2205 samples for 440 Hz at 22.05 kHz,
 * which is exactly 44 periods and 4.4 KB.
 *
 * A tone frequency sharing no factor with the sample rate would demand a table
 * of sampleRate samples — 44 KB — hence the static_assert rather than a silent
 * allocation nobody asked for.
 */
static constexpr uint32_t benchGcd(uint32_t a, uint32_t b) {
    return b == 0 ? a : benchGcd(b, a % b);
}
static constexpr uint32_t BENCH_TONE_LEN =
    CLIENT_SAMPLE_RATE / benchGcd(CLIENT_SAMPLE_RATE, BENCH_TONE_HZ);

static_assert(BENCH_TONE_LEN <= 4096,
              "BENCH_TONE_HZ shares too little with CLIENT_SAMPLE_RATE — the "
              "wrap-exact tone table would be huge. Pick a frequency that "
              "divides more evenly (440 Hz at 22050 Hz needs 2205 samples).");

static int16_t  benchTone[BENCH_TONE_LEN];
static uint32_t benchTonePhase = 0;
static bool     benchToneReady = false;

/**
 * Built once when sourcing starts, not at boot: only a source ever needs it, and
 * on a C3 these are the expensive calls that the table exists to keep out of the
 * transmit path.
 */
static void benchBuildTone() {
    for (uint32_t i = 0; i < BENCH_TONE_LEN; i++) {
        const float t = (float)i / (float)CLIENT_SAMPLE_RATE;
        benchTone[i] = (int16_t)(BENCH_TONE_AMPLITUDE *
                                 sinf(2.0f * (float)M_PI * (float)BENCH_TONE_HZ * t));
    }
    benchToneReady = true;
}
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
        pkt.group = meshId;
        pkt.seq   = txSeq++;
        pkt.len   = ESPNOW_PAYLOAD_SIZE;
        // Table lookup, wrapping by comparison rather than modulo: no division
        // and no float anywhere in the transmit path.
        for (uint32_t i = 0; i < samplesPerPkt; i++) {
            memcpy(pkt.data + i * 2, &benchTone[benchTonePhase], 2);
            if (++benchTonePhase >= BENCH_TONE_LEN) benchTonePhase = 0;
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
    if (!benchToneReady) benchBuildTone();

    benchStartUs   = esp_timer_get_time();
    benchPktIdx    = 0;
    benchSampleIdx = 0;
    benchTonePhase = 0;
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
        "und=%lu dup=%lu rsy=%lu fgn=%lu tx=%lu qfull=%lu senderr=%lu radiofail=%lu "
        "drift=%d ins=%lu drp=%lu dr=%.3f tgt=%d srx=%ld maxalloc=%lu\n",
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
        // Packets that were another mesh's and were dropped unread. Zero on a
        // bench with one household in it, and the evidence that isolation is
        // doing something the moment there are two.
        (unsigned long)rxForeign,
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
        // The level being steered to. It is measured after the settle window
        // rather than computed, so recording it is the only way to tell a
        // correctly calibrated client from one steering at the wrong depth.
        driftCtl.target(),
        // Age of the last received packet, as the silence check sees it. Signed
        // and printed even when it is meaningless (a source has no rx), because
        // a value that goes *negative* or jumps is the evidence that the check
        // is misfiring rather than the radio going quiet.
        lastRxMs ? (long)(millis() - lastRxMs) : -1L,
        // Largest allocatable block. Free heap can sit perfectly still while
        // this one falls, which is exactly what heap fragmentation looks like.
        (unsigned long)ESP.getMaxAllocHeap());
}

static void benchIdentify() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    DEBUG_SERIAL.printf(
        "[BENCH] id fw=%s chip=%s psram=%lu mac=%02X:%02X:%02X:%02X:%02X:%02X "
        "bench=%d bt=%d espnow=%d drift=%d conly=%d mesh=%04X meshname=%s "
        "name=%s\n",
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
        clientOnly ? 1 : 0,
        meshId,
        meshNameForLog(),
        ROOM_NAME);
}

/**
 * Read the rest of a command line, for the one command that takes an argument.
 *
 * Bounded by a short deadline rather than by a newline alone, so a bare `g`
 * typed into a serial monitor that sends no line ending still answers instead
 * of hanging. It costs a quarter second of blocked loop() on that one
 * keystroke, which is why nothing in the audio path may use this.
 */
static size_t benchReadLine(char *out, size_t outSize) {
    size_t n = 0;
    const unsigned long deadline = millis() + 250;
    while ((long)(millis() - deadline) < 0) {
        while (DEBUG_SERIAL.available()) {
            const int c = DEBUG_SERIAL.read();
            if (c == '\n' || c == '\r') {
                out[n] = '\0';
                return n;
            }
            if (n + 1 < outSize) out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    return n;
}

/**
 * Single-character commands, so the host side needs no protocol.
 *
 *   ?  identify          b  reboot into bench mode     n  reboot into normal mode
 *   s  start sourcing    x  stop sourcing              r  report now
 *   d  toggle clock-drift correction
 *   c  toggle client-only mode and reboot (persists across power cuts)
 *   g  print the mesh identity; `g<name>` sets it (persists, no reboot)
 *   p  listen for an offer and join that mesh (the speaker half of pairing)
 *   o  offer this mesh to a node that is listening (the server half)
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
            case 'c': {
                // Persisted, then restarted: whether Bluetooth starts is decided
                // in setup(), so like bench mode this only takes effect on the
                // next boot. Unlike bench mode it is written to NVS, because a
                // node wired into a room should come back as what it was after
                // the power blinks.
                const bool next = !clientOnly;
                prefs.begin(PREF_NAMESPACE, false);
                prefs.putBool(PREF_CLIENT_ONLY, next);
                prefs.end();
                DEBUG_SERIAL.printf("[BENCH] clientonly=%d rebooting\n", next ? 1 : 0);
                DEBUG_SERIAL.flush();
                delay(50);
                ESP.restart();
                break;
            }
            case 'g': {
                // The only command with an argument, hence the line read. No
                // reboot, unlike client-only mode: nothing about the mesh id is
                // decided in setup() -- a transmitter stamps it per packet and a
                // receiver compares it per packet.
                char line[MESH_NAME_MAX * 2 + 2];
                benchReadLine(line, sizeof(line));
                if (line[0] != '\0') {
                    if (meshSetName(line)) {
                        // Whatever this node was receiving, it was receiving it
                        // from a mesh it no longer belongs to.
                        if (!benchSource) enterDiscovery();
                    } else {
                        DEBUG_SERIAL.println("[MESH] error reason=empty_name");
                    }
                }
                DEBUG_SERIAL.printf("[MESH] mesh=%04X name=%s\n",
                                    meshId, meshNameForLog());
                break;
            }
            case 'p': meshStartPairing();  break;
            case 'o': meshStartOffering(); break;
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

    // Before the banner, because the banner reports it, and before
    // setupESPNow() and any decision about Bluetooth, because both depend on it.
    // Read-only handle — the only writer is the 'c' command.
    prefs.begin(PREF_NAMESPACE, true);
    clientOnly = prefs.getBool(PREF_CLIENT_ONLY, false);
    prefs.end();

    // Before the radio, because the receive callback compares every packet
    // against meshId from the first one that arrives.
    meshLoadIdentity();

#if MESH_PAIR_BUTTON_PIN >= 0
    // The BOOT button on these devkits has an external pull-up already; asking
    // for the internal one as well costs nothing and makes the pin safe if this
    // is later moved to a bare GPIO with a button to ground.
    pinMode(MESH_PAIR_BUTTON_PIN, INPUT_PULLUP);
#endif

    DEBUG_SERIAL.println();
    DEBUG_SERIAL.println("================================");
    DEBUG_SERIAL.println("  SonoLoco — Multi-Room Audio");
    DEBUG_SERIAL.println("  Firmware: " FW_VERSION);
#ifdef ENABLE_BLUETOOTH
    if (clientOnly) {
        DEBUG_SERIAL.println("  Mode: CLIENT only (configured — BT never starts)");
    } else {
        DEBUG_SERIAL.println("  Mode: SERVER capable (BT + ESP-NOW)");
    }
#else
    DEBUG_SERIAL.println("  Mode: CLIENT only (no BT Classic on this chip)");
#endif
    DEBUG_SERIAL.printf("  Mesh: %s (id %04X)\n", meshNameForLog(), meshId);
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
    dcfg.targetBytes    = DRIFT_TARGET_BYTES;
    dcfg.targetCeilBytes = DRIFT_TARGET_CEIL_BYTES;
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
    if (psramFound()) {
        uint8_t *ext = (uint8_t *)ps_malloc(JITTER_BUF_SIZE);
        if (ext) jStorage = ext;
    }
    if (!jbuf.init(jStorage, JITTER_BUF_SIZE)) {
        LOG_ERROR("Jitter buffer init failed — JITTER_BUF_SIZE must be a power of two");
    }

    // ESP-NOW must be up before BT to ensure coexistence layer is ready
    setupESPNow();

#ifdef ENABLE_BLUETOOTH
    if (btAllowed()) startBluetooth();
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
    meshServiceButton();
    meshServicePairing();
    meshServiceOffer();

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
            // A client-only node never gets here at all.
            if (btAllowed()) {
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

            // Read both once, and compare SIGNED.
            //
            // lastRxMs is written by the ESP-NOW receive callback on another
            // task. At 220 packets/s a packet lands between this read of
            // millis() and the read of lastRxMs often enough to matter, and the
            // timestamp is then a millisecond in the *future*. Unsigned, that
            // difference wraps to ~4.29e9 and sails past the threshold, so the
            // node declares five seconds of silence in the middle of a flawless
            // stream, drops to DISCOVERY and re-prefills the jitter buffer.
            // Audible, and it invalidates any drift measurement taken across it.
            //
            // Measured, not theorised: [BENCH] silence now=13822 last=13823
            // delta=4294967295 rx=14, and twice more in a 600 s run whose rx
            // counter was advancing by 221 packets every second throughout.
            //
            // Same idiom as clientRetryAfterMs below. Any comparison against a
            // timestamp another task writes needs it; the ones that only ever
            // compare against loop()'s own bookkeeping do not.
            {
            const unsigned long nowMs  = millis();
            const unsigned long lastMs = lastRxMs;
            const long          ageMs  = (long)(nowMs - lastMs);
            if (lastMs > 0 && ageMs > (long)ESPNOW_SILENCE_TIMEOUT_MS) {
                DEBUG_SERIAL.printf("[BENCH] silence now=%lu last=%lu age=%ld rx=%lu\n",
                                    nowMs, lastMs, ageMs, (unsigned long)rxCount);
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
            // maxalloc is the largest single block still allocatable. Free heap
            // alone cannot answer the question the LOG_* String concatenation
            // raises: fragmentation shows up as this number falling while free
            // heap stays flat, so a run that watched only `heap` would report
            // "no change" and prove nothing.
            LOG_INFO(String("Status: mode=") + modeStr +
                     " heap=" + String(ESP.getFreeHeap()) +
                     " maxalloc=" + String(ESP.getMaxAllocHeap()) +
                     " tgt=" + String(driftCtl.target()) +
                     " ins=" + String(driftCtl.inserted) +
                     " drp=" + String(driftCtl.dropped) +
                     " jitter=" + String(jbuf.fill()) + "B" +
                     " rx=" + String(rxCount) +
                     " lost=" + String(seqTracker.lost) +
                     " ovf=" + String(rxOverflow) +
                     " und=" + String(rxUnderrun) +
                     " dup=" + String(seqTracker.dupe) +
                     " rsy=" + String(seqTracker.resync) +
                     " fgn=" + String(rxForeign));
        } else if (currentMode == MODE_SERVER) {
            LOG_INFO(String("Status: mode=") + modeStr +
                     " heap=" + String(ESP.getFreeHeap()) +
                     " maxalloc=" + String(ESP.getMaxAllocHeap()) +
                     " qfull=" + String(txQueueFull) +
                     " senderr=" + String(txSendErr) +
                     " radiofail=" + String(txRadioFail));
        } else {
            // fgn on the DISCOVERY line too, because this is where a node sits
            // when its mesh id is wrong: a stream it can hear perfectly and
            // correctly refuses looks exactly like no stream at all otherwise.
            LOG_INFO(String("Status: mode=") + modeStr +
                     " heap=" + String(ESP.getFreeHeap()) +
                     " maxalloc=" + String(ESP.getMaxAllocHeap()) +
                     " mesh=" + String(meshId, HEX) +
                     " fgn=" + String(rxForeign));
        }
    }
#endif
}
