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
#include "adpcm.h"
#include "drift.h"
#include "holes.h"
#include "jitter.h"
#include "mesh.h"
#include "seqtracker.h"

#ifdef ENABLE_BLUETOOTH
#include "BluetoothA2DPSink.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_coexist.h>
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

// This node's own speaker, and nothing else. Neither setting touches what is
// received, forwarded, buffered or corrected -- only the samples handed to I2S.
//
// `m` mutes, until reboot: on a server the A2DP library's local output (the
// mesh still gets every packet), on a client its I2S. It is how a microphone
// next to one node hears that node alone, and how a bench run stays silent.
static volatile bool outputMuted   = false;
// `M` mixes a client's stereo to mono on both channels, kept in NVS: a node
// with one speaker -- a MAX98357A plays one channel -- would otherwise lose
// whatever was panned to the other.
static bool          clientMonoOut = false;

static const char *PREF_NAMESPACE = "sonoloco";
static const char *PREF_CLIENT_ONLY = "clientonly";
static const char *PREF_MONO_OUT = "monoout";
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
//
// The payload is MESH_BLOCKS_PER_PACKET ADPCM blocks (lib/adpcm): the block
// numbered `seq`, then the one numbered `seq - distance`, which is how a client
// rebuilds a packet it missed from a later one (D13). `len` says how many
// bytes are there -- the first packets of a stream have no older block to
// carry -- and, in its upper bits, the distance and whether this is a repeat.
typedef struct __attribute__((packed)) {
    uint16_t group;
    uint16_t seq;
    uint16_t len;
    uint8_t  data[ESPNOW_PAYLOAD_SIZE];
} AudioPacket;  // 246 bytes total, under the 250-byte ESP-NOW limit

static_assert(ESPNOW_HEADER_SIZE + ESPNOW_PAYLOAD_SIZE <= 250,
              "ESP-NOW cannot send more than 250 bytes per frame");
static_assert(MESH_BLOCK_BYTES == adpcmBlockBytes(MESH_BLOCK_FRAMES),
              "MESH_BLOCK_BYTES must match lib/adpcm's block layout");
static_assert(MESH_BLOCK_FRAMES <= ADPCM_MAX_FRAMES, "block too long for the encoder");

// `len` packs three fields; a payload is at most 244 bytes, so its low byte
// holds the length and the rest is free.
//   bits 0-7   payload bytes. Mask with ESPNOW_LEN_BYTES before using `len`
//              as a length anywhere -- unmasked it is tens of kilobytes.
//   bits 8-14  the distance: the second block's seq is `seq - distance`. On
//              the wire so a server can change it (`D<n>`) and every client
//              follows without a reflash.
//   bit 15     set on the second and later copies of a frame (ESPNOW_TX_COPIES).
//              The receiver needs it only for its counters: a copy whose
//              original arrived is dropped either way, but it was meant, and
//              must not read as a duplicate nobody sent.
#define ESPNOW_LEN_BYTES       0x00FF
#define ESPNOW_LEN_DIST_SHIFT  8
#define ESPNOW_LEN_DIST_MAX    0x7F
#define ESPNOW_LEN_REPEAT      0x8000
static_assert(ESPNOW_PAYLOAD_SIZE <= ESPNOW_LEN_BYTES, "payload length must fit len's low byte");
static_assert(MESH_TX_HISTORY <= ESPNOW_LEN_DIST_MAX + 1, "distance must fit len's bits 8-14");
static_assert(MESH_TX_HISTORY <= HoleTable::SLOTS,
              "clients must remember holes as far back as servers reach");
static_assert((MESH_TX_HISTORY & (MESH_TX_HISTORY - 1)) == 0, "MESH_TX_HISTORY must be a power of two");
static_assert(MESH_REDUNDANCY_DISTANCE < MESH_TX_HISTORY, "the distance must be inside the history");

// XORed into the mesh id on the wire. A node on another audio format sees this
// format's packets as somebody else's mesh and drops them, and the other way
// round -- which matters, because the alternative is worse than silence: a
// node from before ADPCM would play these bytes as raw PCM, full-scale noise
// through whatever amp it has. `fgn=` climbing on a node that should be
// playing is the sign of a node left on the old firmware. Change it whenever
// the payload changes meaning. XOR is its own inverse, so one helper both
// stamps and reads. 0xAD01 was ADPCM with the previous block; 0xAD02 carries
// the distance in `len`.
#define MESH_WIRE_FORMAT   0xAD02
static inline uint16_t wireGroup(uint16_t id) { return id ^ MESH_WIRE_FORMAT; }

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- TX (SERVER mode) ---
static QueueHandle_t     txQueue      = nullptr;
static SemaphoreHandle_t txDone       = nullptr;   // radio is free for the next frame
static uint16_t          txSeq        = 0;
static volatile bool     txReady      = false;   // true while BT audio is streaming
static unsigned long     audioStartMs = 0;       // when BT audio last started

// TX counters — incremented from callbacks, reported from loop(). Never log
// from the send callback itself: it fires ~220x/s and a blocking Serial write
// at that rate causes the very dropouts it would be reporting.
static volatile uint32_t txQueueFull  = 0;
static volatile uint32_t txSendErr    = 0;
// Frames handed to the radio, from whichever source (A2DP or the bench tone).
// The SERVER status line prints it: without it there was no way to tell a
// server that decodes and plays locally from one that also broadcasts.
static volatile uint32_t txSent       = 0;
static volatile uint32_t txRadioFail  = 0;

// How long each frame took from esp_now_send() to its send callback, in
// buckets of <0.5, <1, <2, <5, <10, <20, <50 and >=50 ms. On a server that
// also runs Bluetooth this is where the coexistence arbiter shows: a frame
// the radio could not have for a while waits here. Printed and reset by `a`.
static volatile uint32_t txLatHist[8];
static volatile uint32_t txLatMaxUs   = 0;
static uint32_t          txStartUs    = 0;

// Each frame is handed to the radio this many times; see ESPNOW_TX_COPIES.
// `t<n>` changes it until reboot, so one Bluetooth session can compare.
static volatile uint8_t  txRepeat     = ESPNOW_TX_COPIES;

// Minimum spacing of audio packets (ESPNOW_TX_PACE_US); `P<us>` changes it.
// The wait is an esp_timer, not vTaskDelay: a 1 ms tick would round 2.3 ms up
// to 3 and fall behind the stream.
static volatile uint32_t txPaceUs     = ESPNOW_TX_PACE_US;
static esp_timer_handle_t txPaceTimer = nullptr;
static SemaphoreHandle_t  txPaceTick  = nullptr;

// The one place audio becomes packets, whoever the source is -- A2DP on a
// server, the synthetic tone on a bench source. Frames go into the encoder;
// each completed block goes out with an older one behind it, `txDistance`
// blocks back (MESH_REDUNDANCY_DISTANCE; `D<n>` changes it until reboot).
static AdpcmStereoEncoder txEncoder;
static uint8_t            txHistory[MESH_TX_HISTORY][MESH_BLOCK_BYTES];   // by seq
static uint16_t           txHistoryBlocks = 0;   // blocks since meshTxReset, saturating
static volatile uint8_t   txDistance      = MESH_REDUNDANCY_DISTANCE;

/** Start a new stream: no state carried over, and no older block to send. */
static void meshTxReset() {
    txEncoder.reset();
    txHistoryBlocks = 0;
}

/**
 * Add one stereo frame to the outgoing stream. Returns true when it completed
 * a packet, which has then been queued (or counted as `qfull`).
 */
static bool meshTxFrame(int16_t left, int16_t right) {
    if (!txEncoder.push(left, right)) return false;

    AudioPacket pkt;
    pkt.group = wireGroup(meshId);
    pkt.seq   = txSeq++;
    memcpy(pkt.data, txEncoder.block(), MESH_BLOCK_BYTES);
    uint16_t len = MESH_BLOCK_BYTES;
    const uint8_t d = txDistance;
    if (d > 0 && txHistoryBlocks >= d) {
        memcpy(pkt.data + MESH_BLOCK_BYTES,
               txHistory[(uint16_t)(pkt.seq - d) & (MESH_TX_HISTORY - 1)], MESH_BLOCK_BYTES);
        len += MESH_BLOCK_BYTES;
    }
    pkt.len = len | (uint16_t)(d << ESPNOW_LEN_DIST_SHIFT);
    memcpy(txHistory[pkt.seq & (MESH_TX_HISTORY - 1)], txEncoder.block(), MESH_BLOCK_BYTES);
    if (txHistoryBlocks < 0xFFFF) txHistoryBlocks++;

    if (xQueueSend(txQueue, &pkt, 0) != pdTRUE) txQueueFull++;
    return true;
}

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
// Stereo frames are 4 bytes. The ring is only ever pushed, advanced and
// peeked in whole frames; one partial frame would swap left and right for
// the rest of the stream.
static_assert(JITTER_PREFILL % CLIENT_FRAME_BYTES == 0, "prefill must be whole frames");
static_assert(MESH_BLOCK_FRAMES * CLIENT_FRAME_BYTES < JITTER_BUF_SIZE / 4,
              "a decoded block must be small against the ring");

// The ring's storage, allocated in setup(): PSRAM where there is PSRAM,
// because a WROVER server needs its internal DRAM for the BT stack and the
// ring is only ever touched from task context (the ESP-NOW receive callback
// and loop()), where PSRAM is fine; the heap elsewhere. It used to be a static
// array, which sat in internal DRAM even on boards that then put the ring in
// PSRAM -- 8 KB then, and it is 32 KB now.
static uint8_t     *jStorage = nullptr;
static JitterBuffer jbuf;
static SeqTracker   seqTracker(SEQ_RESYNC_THRESHOLD, MAX_GAP_FILL_PKTS);
// Where each lost block's silence went, for the later packet that carries it.
// Positions in jbuf: cleared with it, and on a resync, which renumbers.
static HoleTable    rxHoles;

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
// Lengths of the runs of consecutive lost packets: 1..7, and 8 or more. Loss
// in single packets is what redundancy or a repeat can recover; loss in long
// bursts is not. Printed and reset by `l`.
static volatile uint32_t rxLossRuns[8];
// Blocks whose original frame was lost and whose repeat copy arrived instead:
// each one is a hole ESPNOW_TX_COPIES filled. `rec` on the status lines.
static volatile uint32_t rxRecovered = 0;

// Lock onto the first node we hear so two simultaneous servers can never
// interleave their streams into one jitter buffer.
static uint8_t          lockedSender[6] = {0};
static volatile bool    senderLocked    = false;

static void espnowTxTask(void *) {
    AudioPacket pkt;
    uint32_t lastAudioUs = 0;
    while (true) {
        if (xQueueReceive(txQueue, &pkt, portMAX_DELAY) != pdTRUE) continue;

        if (pkt.len && txPaceUs && txPaceTimer &&
            uxQueueMessagesWaiting(txQueue) < ESPNOW_TX_PACE_BACKLOG) {
            const uint32_t since = micros() - lastAudioUs;
            if (since < txPaceUs) {
                esp_timer_start_once(txPaceTimer, txPaceUs - since);
                xSemaphoreTake(txPaceTick, pdMS_TO_TICKS(50));
            }
        }
        if (pkt.len) lastAudioUs = micros();

        const uint8_t copies = pkt.len ? txRepeat : 1;   // beacons go once
        for (uint8_t c = 0; c < copies; c++) {
            if (c == 1) pkt.len |= ESPNOW_LEN_REPEAT;
            // Wait for the previous frame to actually leave the radio. Firing
            // esp_now_send() back-to-back at ~220 pkt/s returns
            // ESP_ERR_ESPNOW_NO_MEM and drops audio without any indication.
            xSemaphoreTake(txDone, pdMS_TO_TICKS(50));

            txStartUs = micros();
            esp_err_t err = esp_now_send(BROADCAST_ADDR, (uint8_t *)&pkt,
                                         ESPNOW_HEADER_SIZE + (pkt.len & ESPNOW_LEN_BYTES));
            if (err != ESP_OK) {
                txSendErr++;
                xSemaphoreGive(txDone);   // no callback will come; release the gate
            } else {
                txSent++;
            }
        }
    }
}

static void onEspNowSent(const uint8_t *, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) txRadioFail++;
    const uint32_t us = micros() - txStartUs;
    if (us > txLatMaxUs) txLatMaxUs = us;
    static const uint32_t edges[7] = {500, 1000, 2000, 5000, 10000, 20000, 50000};
    int b = 0;
    while (b < 7 && us >= edges[b]) b++;
    txLatHist[b]++;
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
    const bool     repeat = (plen & ESPNOW_LEN_REPEAT) != 0;
    const uint16_t dist   = (plen >> ESPNOW_LEN_DIST_SHIFT) & ESPNOW_LEN_DIST_MAX;
    plen &= ESPNOW_LEN_BYTES;

    const bool beacon = meshIsBeacon(seq, plen);
    const uint16_t id = wireGroup(group);   // a node on another format lands elsewhere

    // Whose mesh is this? Checked BEFORE the sender lock below, and the order is
    // the whole point: a neighbour's server that took the lock would leave this
    // node ignoring its own household until it next fell back to DISCOVERY.
    if (id != meshId) {
        // Adoption happens only from a beacon -- somebody is holding the button
        // on a node of that mesh right now. Adopting from any foreign *stream*,
        // as this first did, let a neighbour capture a node by doing nothing
        // more deliberate than playing music inside the pairing window.
        //
        // Only the intent is recorded here: the NVS write happens in loop(),
        // because this is the WiFi task and a flash erase here would stall the
        // radio mid-stream.
        if (beacon && pairing && !pairCandidateReady) {
            pairCandidate      = id;
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

    // Trust the wire for nothing: whole blocks, as many as the header says and
    // no more than were received or than a packet can hold. Anything else is
    // not a packet this firmware sent.
    const int blocks = (int)plen / MESH_BLOCK_BYTES;
    if (blocks < 1 || blocks > MESH_BLOCKS_PER_PACKET ||
        (int)plen != blocks * MESH_BLOCK_BYTES ||
        (int)plen > len - ESPNOW_HEADER_SIZE) return;
    const uint8_t *payload = data + ESPNOW_HEADER_SIZE;

    lastRxMs = millis();

    if (currentMode == MODE_DISCOVERY) {
        rxCount++;
        rxActive = true;   // signal main loop to switch to CLIENT
        return;
    }

    if (currentMode != MODE_CLIENT) return;

    // Duplicate / reordered / restarted-server handling, including the unsigned
    // 65535-gap trap, is in SeqTracker and covered by the host tests.
    const uint32_t lostBefore   = seqTracker.lost;
    const uint32_t resyncBefore = seqTracker.resync;
    const SeqResult sr = seqTracker.update(seq, repeat);
    if (!sr.accept) return;   // exact retransmit — replaying it is an audible stutter
    if (seqTracker.resync != resyncBefore) rxHoles.clear();   // numbering restarted
    // Blocks, not frames: with every frame sent twice, counting frames would
    // make rx twice the source's tx and halve every loss percentage.
    rxCount++;
    if (repeat) rxRecovered++;   // the original was lost and the copy stood in
    const uint32_t run = seqTracker.lost - lostBefore;
    if (run) rxLossRuns[run < 8 ? run - 1 : 7]++;

    // What was missed goes in as silence, in playing order, so playback keeps
    // its timing instead of splicing the stream shorter on every loss -- and
    // each block's place is remembered, for the packet that carries it later.
    static const int BLOCK_PCM = MESH_BLOCK_FRAMES * CLIENT_FRAME_BYTES;
    for (int i = sr.fillPackets; i > 0; i--) {
        const int pos = jbuf.writePos();
        if (jbuf.pushSilence(BLOCK_PCM)) rxHoles.add((uint16_t)(seq - i), pos);
        else rxOverflow++;
    }

    // Static: this is the WiFi task's stack, and it is not ours to spend.
    static int16_t pcm[2 * MESH_BLOCK_FRAMES];
    if (adpcmDecodeStereoBlock(payload, MESH_BLOCK_FRAMES, pcm)) {
        if (!jbuf.pushBlock((const uint8_t *)pcm, BLOCK_PCM)) rxOverflow++;
    } else if (!jbuf.pushSilence(BLOCK_PCM)) {
        rxOverflow++;
    }

    // The older block this packet carries, over its silence if it was lost and
    // has not played. The guard keeps the write clear of the batch the I2S
    // side may be copying out right now.
    int pos;
    if (blocks >= 2 && dist > 0 && rxHoles.take((uint16_t)(seq - dist), &pos) &&
        adpcmDecodeStereoBlock(payload + MESH_BLOCK_BYTES, MESH_BLOCK_FRAMES, pcm) &&
        jbuf.patch(pos, (const uint8_t *)pcm, BLOCK_PCM, CLIENT_BATCH * CLIENT_FRAME_BYTES)) {
        seqTracker.recovered(1);   // played, so not a hole
        rxRecovered++;
        rxCount++;                 // rx + lost still adds up to the source's tx
    }
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

    txPaceTick = xSemaphoreCreateBinary();
    if (txPaceTick) {
        esp_timer_create_args_t pa = {};
        pa.callback = [](void *) { xSemaphoreGive(txPaceTick); };
        pa.name     = "tx_pace";
        if (esp_timer_create(&pa, &txPaceTimer) != ESP_OK) txPaceTimer = nullptr;
    }

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


// Off with `f` over serial: the server keeps playing locally and stops handing
// frames to the radio, so one Bluetooth session can be measured with and
// without the mesh's transmissions competing for the radio.
static volatile bool a2dpForward = true;

// How the Bluetooth task spends its time, as seen from the two hooks the
// library gives us: the stream reader runs after a packet is decoded and
// before the library's blocking i2s_write(), data_received runs after it.
//
//   write = data_received - end of our callback: time blocked in i2s_write()
//   idle  = our callback - previous data_received: waiting for and decoding
//           the next packet
//   gap   = end of our callback - previous data_received: idle plus the
//           forwarding in our callback, i.e. everything between one
//           i2s_write() and the next
//
// i2s_write() returns once the packet's tail fits in the DMA ring, so -- as
// long as writes block, which `nb` checks -- the ring is full at that moment
// and holds serverDmaMs() of audio. A gap longer than that is a stretch in
// which the DMA ran dry and played zeros (the library sets
// tx_desc_auto_clear) -- an audible hole, counted as `late`.
//
// Writes only block if the ring is already full. With the library's 11.6 ms
// ring that is automatic: a 23.2 ms packet cannot fit. A ring deeper than a
// packet stays full only if something filled it, which is what
// serverPrefill() does at the start of every stream; `nb` counts writes that
// returned without blocking, i.e. moments the ring was not full.
// The first version of this histogram measured idle alone, and with
// forwarding on the callback's own 5 ms went uncounted. Printed and reset by
// `a`.
static volatile uint32_t a2dpPackets    = 0;
static volatile uint32_t a2dpBytes      = 0;
static volatile uint32_t a2dpPktMin     = UINT32_MAX;
static volatile uint32_t a2dpPktMax     = 0;
static volatile uint32_t a2dpIdleMaxUs  = 0;
static volatile uint32_t a2dpGapMaxUs   = 0;
static volatile uint32_t a2dpWriteMaxUs = 0;
static volatile uint32_t a2dpCbMaxUs    = 0;
static volatile uint32_t a2dpLate       = 0;
static volatile uint32_t a2dpNoBlock    = 0;
static volatile bool     a2dpPrefill    = false;
static volatile bool     jingleActive   = false;   // see playJingleOverA2DP()
static volatile uint32_t a2dpGapHist[8];   // 5 ms buckets, last one open-ended
static uint32_t          a2dpCbEndUs    = 0;
static uint32_t          a2dpWriteEndUs = 0;
static unsigned long     a2dpWindowMs   = 0;

// The DMA ring the library's I2S driver was installed with. SERVER_DMA_BUF_LEN
// at boot; `q<frames>` reinstalls it between streams, so one Bluetooth session
// can compare two depths without a reflash and a reconnect in between.
static int serverDmaLen = SERVER_DMA_BUF_LEN;

static float serverDmaMs() {
    return SERVER_DMA_BUF_COUNT * serverDmaLen * 1000.0f / BT_SAMPLE_RATE;
}

// The library's own default config (BluetoothA2DPOutputLegacy) with only the
// DMA ring changed. The sample rate is overwritten on codec configuration.
static i2s_config_t serverI2SConfig(int dmaLen) {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = BT_SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = 0;
    cfg.dma_buf_count        = SERVER_DMA_BUF_COUNT;
    cfg.dma_buf_len          = dmaLen;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    return cfg;
}

static void a2dpStatsReset() {
    a2dpPackets = a2dpBytes = a2dpLate = a2dpNoBlock = 0;
    a2dpPktMin = UINT32_MAX;
    a2dpPktMax = a2dpIdleMaxUs = a2dpGapMaxUs = a2dpWriteMaxUs = a2dpCbMaxUs = 0;
    for (auto &h : a2dpGapHist) h = 0;
    for (auto &h : txLatHist) h = 0;
    txLatMaxUs = 0;
    a2dpWriteEndUs = 0;   // the first packet after a reset has no gap to measure
    a2dpWindowMs = millis();
}

static void a2dpStatsPrint() {
    const float secs = (millis() - a2dpWindowMs) / 1000.0f;
    DEBUG_SERIAL.printf(
        "[A2DP] win=%.1fs pk=%lu pk/s=%.1f pkB=%lu..%lu ring=%.1fms late=%lu nb=%lu "
        "gapmax=%.1fms idlemax=%.1fms cbmax=%.2fms writemax=%.1fms fwd=%d wifi=%d "
        "heap=%lu gap5ms=",
        secs, (unsigned long)a2dpPackets, a2dpPackets / secs,
        (unsigned long)(a2dpPackets ? a2dpPktMin : 0), (unsigned long)a2dpPktMax,
        serverDmaMs(), (unsigned long)a2dpLate, (unsigned long)a2dpNoBlock,
        a2dpGapMaxUs / 1000.0f, a2dpIdleMaxUs / 1000.0f, a2dpCbMaxUs / 1000.0f,
        a2dpWriteMaxUs / 1000.0f, a2dpForward ? 1 : 0, espnowActive ? 1 : 0,
        (unsigned long)ESP.getFreeHeap());
    for (int i = 0; i < 8; i++)
        DEBUG_SERIAL.printf(i ? ",%lu" : "%lu", (unsigned long)a2dpGapHist[i]);
    DEBUG_SERIAL.printf(" tx=%lu txlat=", (unsigned long)txSent);
    for (int i = 0; i < 8; i++)
        DEBUG_SERIAL.printf(i ? ",%lu" : "%lu", (unsigned long)txLatHist[i]);
    DEBUG_SERIAL.printf(" txlatmax=%.1fms rep=%u dist=%u pace=%lu mute=%d qfull=%lu\n",
                        txLatMaxUs / 1000.0f, txRepeat, txDistance, (unsigned long)txPaceUs,
                        outputMuted ? 1 : 0, (unsigned long)txQueueFull);
}

static void a2dpWriteDone() {
    const uint32_t now = micros();
    const uint32_t w = now - a2dpCbEndUs;
    if (w > a2dpWriteMaxUs) a2dpWriteMaxUs = w;
    if (w < 1000) a2dpNoBlock++;
    a2dpWriteEndUs = now;
}

// Fill the whole DMA ring with silence before the first packet of a stream,
// from the BT task, so the library's writes queue behind a full ring from the
// start instead of running just in time. Costs the ring's depth in latency,
// once; see SERVER_DMA_BUF_LEN in config.h.
static void serverPrefill() {
    static const uint8_t zeros[256] = {0};
    int bytes = SERVER_DMA_BUF_COUNT * serverDmaLen * 4;
    while (bytes > 0) {
        const size_t n = min(bytes, (int)sizeof(zeros));
        if (i2sWriteAll(zeros, n, pdMS_TO_TICKS(50)) < n) break;   // I2S not running
        bytes -= n;
    }
}

static void a2dpForwardPacket(const uint8_t *data, uint32_t length);

// Called from BT task, once per decoded packet, before the library writes it
// to I2S. Accounts for the packet, then forwards it to the mesh.
static void a2dpDataCallback(const uint8_t *data, uint32_t length) {
    // Not while a jingle owns the ring: the silence would land inside it.
    // Left pending, it runs on the first packet after the jingle instead.
    if (a2dpPrefill && !jingleActive) {
        a2dpPrefill = false;
        serverPrefill();
    }
    const uint32_t t0 = micros();
    a2dpPackets++;
    a2dpBytes += length;
    if (length < a2dpPktMin) a2dpPktMin = length;
    if (length > a2dpPktMax) a2dpPktMax = length;

    a2dpForwardPacket(data, length);

    a2dpCbEndUs = micros();
    if (a2dpCbEndUs - t0 > a2dpCbMaxUs) a2dpCbMaxUs = a2dpCbEndUs - t0;
    if (a2dpWriteEndUs) {
        const uint32_t idle = t0 - a2dpWriteEndUs;
        const uint32_t gap  = a2dpCbEndUs - a2dpWriteEndUs;
        if (idle > a2dpIdleMaxUs) a2dpIdleMaxUs = idle;
        if (gap > a2dpGapMaxUs)   a2dpGapMaxUs = gap;
        a2dpGapHist[min<uint32_t>(gap / 5000, 7)]++;
        if (gap > (uint32_t)(serverDmaMs() * 1000.0f)) a2dpLate++;
    }
}

// Encodes the decoded A2DP stream -- 44.1 kHz stereo, after the volume -- into
// mesh packets, frame by frame. The BT library still drives I2S locally.
static void a2dpForwardPacket(const uint8_t *data, uint32_t length) {
    if (currentMode != MODE_SERVER || !txReady || !espnowActive || !a2dpForward) return;
    // Signed, for the reason spelled out at the ESP-NOW silence check: this
    // timestamp is written from the A2DP state callback, and an unsigned
    // difference against a timestamp set a moment in the future wraps to a huge
    // number — here that would silently skip the warmup instead of enforcing it.
    if ((long)(millis() - audioStartMs) < (long)TX_WARMUP_MS) return;

    const int16_t *in     = (const int16_t *)data;
    const int      frames = length / 4;   // 4 bytes per stereo frame
    for (int i = 0; i < frames; i++) meshTxFrame(in[2 * i], in[2 * i + 1]);
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
        txReady        = true;
        audioStartMs   = millis();
        a2dpWriteEndUs = 0;   // the first packet of a stream has no gap
        a2dpPrefill    = true;
        meshTxReset();   // a new stream: nothing from the last one carries over
        LOG_INFO("BT audio started → ESP-NOW TX will activate in " + String(TX_WARMUP_MS) + "ms");
    } else {
        txReady        = false;
        audioStartMs   = 0;
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

static i2s_pin_config_t serverI2SPins() {
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    return pins;
}

static void startBluetooth() {
    if (btSinkStarted) return;

    bringUpBtController();

    a2dpSink.set_pin_config(serverI2SPins());
    a2dpSink.set_i2s_config(serverI2SConfig(serverDmaLen));
    // Output on through start(), whatever the mute: the library installs its
    // I2S driver there only if it is. The mute goes on after; see stopBluetooth().
    a2dpSink.set_stream_reader(a2dpDataCallback, true);  // true = keep local I2S output
    a2dpSink.set_on_data_received(a2dpWriteDone);
    a2dpSink.set_on_connection_state_changed(btConnectionChanged);
    a2dpSink.set_on_audio_state_changed(btAudioChanged);
    a2dpSink.set_auto_reconnect(false);
    a2dpSink.set_volume(VOLUME_DEFAULT);
    a2dpSink.start(BT_DEVICE_NAME);
    if (outputMuted) a2dpSink.set_stream_reader(a2dpDataCallback, false);
    btSinkStarted = true;
    LOG_INFO("BT discoverable as: " BT_DEVICE_NAME);
    LOG_INFO("Heap after BT start: " + String(ESP.getFreeHeap()) + " bytes, maxalloc " +
             String(ESP.getMaxAllocHeap()));
}

/**
 * `q<frames>`: reinstall the A2DP library's I2S driver with a different DMA
 * ring, so one Bluetooth session can compare two depths -- a reflash would
 * drop the link, and getting it back needs a person to click Connect.
 *
 * Only between streams: during one the BT task could be inside i2s_write().
 * The library installs the driver once in start() and, as configured here
 * (set_output_active_by_state never called), leaves it *running* across a
 * suspend, playing zeros -- on resume it sees I2S still active and does not
 * call i2s_start(). So the new driver must be left running too. The first
 * version stopped it, the library's next i2s_write() blocked forever
 * (portMAX_DELAY), and the BT task with it.
 */
static void serverSetDmaLen(int len) {
    if (!btSinkStarted || txReady) {
        DEBUG_SERIAL.println("[A2DP] error reason=streaming (stop playback first)");
        return;
    }
    if (len < 8 || len > 1024) {
        DEBUG_SERIAL.println("[A2DP] error reason=range (8..1024 frames)");
        return;
    }
    i2s_config_t cfg = serverI2SConfig(len);
    cfg.sample_rate = a2dpSink.sample_rate();
    i2s_driver_uninstall(I2S_NUM_0);
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    if (err == ESP_OK) {
        i2s_pin_config_t pins = serverI2SPins();
        i2s_set_pin(I2S_NUM_0, &pins);
        i2s_zero_dma_buffer(I2S_NUM_0);
        i2s_start(I2S_NUM_0);           // running, as the library left it
        a2dpSink.set_i2s_config(cfg);
        serverDmaLen = len;
    }
    DEBUG_SERIAL.printf("[A2DP] dma=%dx%d ring=%.1fms install=%s heap=%lu\n",
                        SERVER_DMA_BUF_COUNT, serverDmaLen, serverDmaMs(),
                        esp_err_to_name(err), (unsigned long)ESP.getFreeHeap());
}

/**
 * Play a connect/disconnect jingle into the I2S driver the A2DP library owns,
 * without the library writing into it at the same time.
 *
 * The jingles are written from loop(); the library writes each decoded packet
 * from the BT task. If a stream is running -- Windows opens one the moment it
 * connects whenever anything on the PC has audio open -- the two interleave in
 * the ring and the jingle comes out chopped and stretched: "the 3 tones were
 * very laggy" (2026-09-28). With the 46 ms ring the stream start also writes
 * a ring of silence (serverPrefill) into the middle of it.
 *
 * So for the jingle's ~0.6 s the library's local output is muted: packets are
 * still decoded, still passed to a2dpDataCallback and still forwarded to the
 * mesh -- they only skip I2S. A write already in progress finishes first (the
 * driver serialises writers), so at most one packet precedes the jingle.
 * Afterwards a running stream restarts behind a ring of silence, as at any
 * stream start. `J` plays a jingle the old, unguarded way, for comparison.
 */
static void playJingleOverA2DP(void (*jingle)()) {
    jingleActive = true;
    a2dpSink.set_stream_reader(a2dpDataCallback, false);
    jingle();
    a2dpSink.set_stream_reader(a2dpDataCallback, !outputMuted);
    if (txReady) a2dpPrefill = true;
    jingleActive = false;
}

static void stopBluetooth() {
    if (!btSinkStarted) return;
    // end(false) keeps the BT controller in memory so start() works again.
    // end(true) releases it, and every later attempt to become a SERVER fails
    // until the node is power-cycled — which breaks the whole "any node can be
    // either role" premise.
    //
    // But end(false) deinitialises only A2DP and AVRCP. Bluedroid and the
    // controller stay up, still page- and inquiry-scanning like a speaker
    // waiting for a phone, and coexistence gives those scans the radio: a
    // WROVER client lost 4.7% of a streaming server's packets, in runs of 4-5
    // and 8+, beside an S3 that lost 2.0% (2026-09-30). So Bluedroid is
    // disabled and the controller switched off too. Disabled, not
    // deinitialised: the library remembers having initialised Bluedroid and
    // never does it again, so after a deinit its start() loops forever on
    // "Failed to enable bluedroid". startBluetooth() re-enables both
    // (bringUpBtController, then the library's own enable).
    //
    // Output back on first: end() uninstalls the library's I2S driver only if
    // it is. A muted node (`m`) kept the driver, and its next CLIENT install
    // failed with ESP_ERR_INVALID_STATE -- every 5 s, stuck in DISCOVERY.
    a2dpSink.set_stream_reader(a2dpDataCallback, true);
    a2dpSink.end(false);
    const esp_err_t e1 = esp_bluedroid_disable();
    const esp_err_t e2 = esp_bt_controller_disable();
    btSinkStarted = false;
    LOG_INFO(String("BT stopped, controller off, memory kept (") + esp_err_to_name(e1) + "/" +
             esp_err_to_name(e2) + ")");
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
    LOG_INFO("I2S initialised for CLIENT mode at " + String(CLIENT_SAMPLE_RATE) + " Hz " +
             (clientMonoOut ? "stereo, mixed to mono" : "stereo"));
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

    // Interleaved L, R -- exactly the ring's layout and exactly what I2S takes.
    static int16_t stereo[CLIENT_BATCH * 2];
    static const int BATCH_BYTES = CLIENT_BATCH * CLIENT_FRAME_BYTES;

    if (!jbuf.peek((uint8_t *)stereo, BATCH_BYTES)) {
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
    // frame either side of a write whose length is already handled correctly,
    // rather than an edit in the middle of a buffer that I2S may only half take.
    DriftController::Correction corr = DriftController::NONE;
    if (driftEnabled) corr = driftCtl.update(millis(), jbuf.fill());

    // After the peek, so what is consumed is untouched: these change only what
    // this node's speaker hears, never the timing or the drift loop.
    if (outputMuted) {
        memset(stereo, 0, sizeof(stereo));
    } else if (clientMonoOut) {
        // One speaker (a MAX98357A plays one channel): both channels, summed.
        for (int i = 0; i < CLIENT_BATCH; i++) {
            const int16_t m = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) >> 1);
            stereo[2 * i] = stereo[2 * i + 1] = m;
        }
    }

    // Blocking with a short timeout paces this loop to the I2S sample clock.
    size_t bw = 0;
    i2s_write(I2S_NUM_0, stereo, BATCH_BYTES, &bw, pdMS_TO_TICKS(20));

    // Consume only what the DMA actually took. An earlier version advanced
    // the read pointer unconditionally, discarding every sample I2S refused —
    // which at loop() speed was most of them.
    //
    // Round down to whole stereo frames first. i2s_write normally returns a
    // multiple of 4 here, but if it ever returned a partial frame, advancing by
    // it would shift the ring's framing by a sample and swap left and right for
    // the rest of the stream -- the exact failure pushBlock exists to prevent.
    const size_t frames = bw / CLIENT_FRAME_BYTES;
    jbuf.advance((int)(frames * CLIENT_FRAME_BYTES));

    // One frame of correction, at a batch boundary -- an edit every half second
    // or so at the offsets these boards actually have. Inaudible, and it needs
    // no resampler. Nothing is counted unless it happened: an I2S write that
    // came up short leaves the correction owed, and it is applied next batch.
    //
    // Both directions require frames > 0, i.e. that playback actually advanced.
    // A correction is a statement about the rate audio is being consumed at, and
    // if the DMA took nothing then it was not consumed at any rate; dropping a
    // frame there would discard audio that was never played, to fix a drift that
    // did not accrue.
    if (corr == DriftController::DROP && frames > 0) {
        // Consume a frame without playing it. A whole frame, both channels, so
        // the ring's framing survives -- the one thing this path must never get
        // wrong.
        if (jbuf.fill() >= CLIENT_FRAME_BYTES) {
            jbuf.advance(CLIENT_FRAME_BYTES);
            driftCtl.confirm(corr);
        }
    } else if (corr == DriftController::INSERT && frames > 0) {
        // Play a frame twice without consuming it: hold it one extra sample
        // period. No discontinuity is possible by construction, because it is
        // the frame that was just played.
        int16_t frame[2] = {stereo[2 * (frames - 1)], stereo[2 * (frames - 1) + 1]};
        size_t  bwExtra  = 0;
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
    rxHoles.clear();
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
    rxRecovered  = 0;
}

static void enterDiscovery() {
    LOG_INFO("=== DISCOVERY ===");

    txReady  = false;
    meshTxReset();

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
    meshTxReset();
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
    rxHoles.clear();
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
    pkt.group = wireGroup(meshId);
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
 * is sampleRate / gcd(sampleRate, toneHz): 2205 samples for 440 Hz at 44.1 kHz,
 * which is exactly 22 periods and 4.4 KB.
 *
 * A tone frequency sharing no factor with the sample rate would demand a table
 * of sampleRate samples — 88 KB — hence the static_assert rather than a silent
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
              "divides more evenly (440 Hz at 44100 Hz needs 2205 samples).");

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

    const int64_t now = esp_timer_get_time();

    // Bounded catch-up: if this node was blocked for a while, send a few packets
    // back to back but never sit here spinning out a whole backlog.
    for (int burst = 0; burst < BENCH_MAX_CATCHUP_PKTS; burst++) {
        const int64_t due = benchStartUs +
            (int64_t)benchPktIdx * 1000000LL * (int64_t)MESH_BLOCK_FRAMES / CLIENT_SAMPLE_RATE;
        if (now < due) return;

        // One packet's worth of frames, through the same encoder a server uses.
        // Table lookup, wrapping by comparison rather than modulo: no division
        // and no float anywhere in the transmit path.
        const uint32_t fullBefore = txQueueFull;
        for (int i = 0; i < MESH_BLOCK_FRAMES; i++) {
            const int16_t s = benchTone[benchTonePhase];
            if (++benchTonePhase >= BENCH_TONE_LEN) benchTonePhase = 0;
            meshTxFrame(s, s);
        }
        benchSampleIdx += MESH_BLOCK_FRAMES;

        if (txQueueFull == fullBefore) benchTxPackets++;
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
    meshTxReset();
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
        "drift=%d ins=%lu drp=%lu dr=%.3f tgt=%d srx=%ld maxalloc=%lu rec=%lu\n",
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
        (unsigned long)ESP.getMaxAllocHeap(),
        // Blocks that played only because their repeat copy arrived.
        (unsigned long)rxRecovered);
}

static void benchIdentify() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    DEBUG_SERIAL.printf(
        "[BENCH] id fw=%s chip=%s psram=%lu mac=%02X:%02X:%02X:%02X:%02X:%02X "
        "bench=%d bt=%d espnow=%d drift=%d conly=%d mono=%d mute=%d mesh=%04X meshname=%s "
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
        clientMonoOut ? 1 : 0,
        outputMuted ? 1 : 0,
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
 *   m  mute this node's speaker until reboot (server: local output; client: I2S)
 *   M  toggle mixing a client's stereo to mono, for one speaker (persists)
 *   l  client: print and reset the lost-run histogram and `rec`
 *   t  `t<n>` sends each mesh frame n times, 1..3, until reboot
 *   P  `P<us>` minimum spacing between audio packets, 0 = none, until reboot
 *   D  `D<n>` each packet also carries the block n packets back, until reboot
 *   R  `R<Mbps>` sets the ESP-NOW PHY rate this node sends at, until reboot
 *
 * Bluetooth server diagnostics, for telling the A2DP side from the mesh side:
 *   k  `k<aa:bb:cc:dd:ee:ff>` dials a bonded A2DP source, as a headset would
 *   V  `V<0..127>` sets the A2DP volume, which the mesh stream carries too
 *   e  `e<n>` coexistence preference: 0 WiFi, 1 Bluetooth, 2 balance
 *   a  print and reset the A2DP timing window (see a2dpDataCallback)
 *   f  toggle forwarding A2DP audio to the mesh; local playback continues
 *   w  stop WiFi altogether, until the next reboot -- what a WROOM server is
 *   q  `q<frames>` sets the local I2S DMA buffer length, between streams only;
 *      bare `q` prints the current ring
 *   j  play the connect jingle as a connection does; `J` the old way, with
 *      the library still writing -- run either during a stream to compare
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
            case 'l':
                DEBUG_SERIAL.printf("[LOSS] rx=%lu lost=%lu rec=%lu runs=", (unsigned long)rxCount,
                                    (unsigned long)seqTracker.lost, (unsigned long)rxRecovered);
                for (int i = 0; i < 8; i++) {
                    DEBUG_SERIAL.printf(i ? ",%lu" : "%lu", (unsigned long)rxLossRuns[i]);
                    rxLossRuns[i] = 0;
                }
                DEBUG_SERIAL.println();
                break;
            case 't': {
                char line[8];
                benchReadLine(line, sizeof(line));
                const int n = atoi(line);
                if (n >= 1 && n <= 3) txRepeat = (uint8_t)n;
                DEBUG_SERIAL.printf("[MESH] repeat=%u\n", txRepeat);
                break;
            }
            case 'D': {
                // `D<n>`: which older block each packet carries, n packets
                // back, until reboot; 0 = none. Clients read it off the wire.
                char line[8];
                benchReadLine(line, sizeof(line));
                const int n = atoi(line);
                if (n >= 0 && n < MESH_TX_HISTORY) txDistance = (uint8_t)n;
                DEBUG_SERIAL.printf("[MESH] distance=%u\n", txDistance);
                break;
            }
            case 'P': {
                // `P<us>`: minimum spacing between audio packets from this
                // node, until reboot; 0 = unpaced. See ESPNOW_TX_PACE_US.
                char line[8];
                benchReadLine(line, sizeof(line));
                const long us = atol(line);
                if (us >= 0 && us <= 10000) txPaceUs = (uint32_t)us;
                DEBUG_SERIAL.printf("[MESH] pace=%luus\n", (unsigned long)txPaceUs);
                break;
            }
            case 'R': {
                // `R<Mbps>`: the ESP-NOW PHY rate this node sends at, until
                // reboot. 1, 2 (DSSS) or 6..54 (OFDM). See ESPNOW_PHY_RATE.
                char line[8];
                benchReadLine(line, sizeof(line));
                static const struct { int mbps; wifi_phy_rate_t r; } rates[] = {
                    {1, WIFI_PHY_RATE_1M_L}, {2, WIFI_PHY_RATE_2M_L},
                    {6, WIFI_PHY_RATE_6M}, {9, WIFI_PHY_RATE_9M}, {12, WIFI_PHY_RATE_12M},
                    {18, WIFI_PHY_RATE_18M}, {24, WIFI_PHY_RATE_24M}, {36, WIFI_PHY_RATE_36M},
                    {48, WIFI_PHY_RATE_48M}, {54, WIFI_PHY_RATE_54M}};
                const int want = atoi(line);
                esp_err_t err = ESP_ERR_INVALID_ARG;
                for (const auto &e : rates)
                    if (e.mbps == want) err = esp_wifi_config_espnow_rate(WIFI_IF_STA, e.r);
                DEBUG_SERIAL.printf("[MESH] rate=%dM -> %s\n", want, esp_err_to_name(err));
                break;
            }
            case 'm':
                outputMuted = !outputMuted;
#ifdef ENABLE_BLUETOOTH
                if (btSinkStarted) a2dpSink.set_stream_reader(a2dpDataCallback, !outputMuted);
#endif
                DEBUG_SERIAL.printf("[OUT] mute=%d\n", outputMuted ? 1 : 0);
                break;
            case 'M':
                clientMonoOut = !clientMonoOut;
                prefs.begin(PREF_NAMESPACE, false);
                prefs.putBool(PREF_MONO_OUT, clientMonoOut);
                prefs.end();
                DEBUG_SERIAL.printf("[OUT] mono=%d\n", clientMonoOut ? 1 : 0);
                break;
            case 'p': meshStartPairing();  break;
            case 'o': meshStartOffering(); break;
#ifdef ENABLE_BLUETOOTH
            case 'a': a2dpStatsPrint(); a2dpStatsReset(); break;
            case 'f':
                a2dpForward = !a2dpForward;
                DEBUG_SERIAL.printf("[A2DP] fwd=%d\n", a2dpForward ? 1 : 0);
                break;
            case 'w':
                // One way only: bringing ESP-NOW back after esp_wifi_stop()
                // means redoing channel and PHY rate, and a reboot does that.
                a2dpForward  = false;
                espnowActive = false;
                DEBUG_SERIAL.printf("[A2DP] wifi stop -> %s\n", esp_err_to_name(esp_wifi_stop()));
                break;
            case 'j':
                if (!btSinkStarted) break;
                DEBUG_SERIAL.printf("[A2DP] jingle guarded=1 streaming=%d\n", txReady ? 1 : 0);
                playJingleOverA2DP(playConnectedSound);
                break;
            case 'J':
                if (!btSinkStarted) break;
                DEBUG_SERIAL.printf("[A2DP] jingle guarded=0 streaming=%d\n", txReady ? 1 : 0);
                playConnectedSound();
                break;
            case 'V': {
                // `V<0..127>`: the A2DP volume, as a phone's slider would set
                // it -- applied before forwarding, so it moves every room. A
                // node that dialled the PC with `k` starts at VOLUME_DEFAULT.
                char line[8];
                benchReadLine(line, sizeof(line));
                if (line[0] != '\0') a2dpSink.set_volume((uint8_t)constrain(atoi(line), 0, 127));
                DEBUG_SERIAL.printf("[A2DP] volume=%d\n", a2dpSink.get_volume());
                break;
            }
            case 'e': {
                // Coexistence preference, at runtime, so one Bluetooth session
                // can compare them: 0 WiFi, 1 Bluetooth, 2 balance (the default).
                char line[8];
                benchReadLine(line, sizeof(line));
                if (line[0] != '\0') {
                    const int p = atoi(line);
                    DEBUG_SERIAL.printf("[A2DP] coex prefer=%d -> %s\n", p,
                        esp_err_to_name(esp_coex_preference_set((esp_coex_prefer_t)p)));
                }
                break;
            }
            case 'k': {
                // `k<aa:bb:cc:dd:ee:ff>`: dial an A2DP source that is already
                // bonded, the way a headset reconnects to a phone. What lets a
                // bench reflash a server without a person clicking Connect.
                char line[24];
                benchReadLine(line, sizeof(line));
                unsigned b[6];
                if (sscanf(line, "%x:%x:%x:%x:%x:%x",
                           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
                    esp_bd_addr_t addr;
                    for (int i = 0; i < 6; i++) addr[i] = (uint8_t)b[i];
                    DEBUG_SERIAL.printf("[A2DP] connect %s -> %d\n", line,
                                        a2dpSink.connect_to(addr) ? 1 : 0);
                } else {
                    DEBUG_SERIAL.println("[A2DP] error reason=address (k aa:bb:cc:dd:ee:ff)");
                }
                break;
            }
            case 'q': {
                char line[8];
                benchReadLine(line, sizeof(line));
                if (line[0] != '\0') serverSetDmaLen(atoi(line));
                else DEBUG_SERIAL.printf("[A2DP] dma=%dx%d ring=%.1fms\n", SERVER_DMA_BUF_COUNT,
                                         serverDmaLen, serverDmaMs());
                break;
            }
#endif
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
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // Native USB serial (S3, C3). By default write() waits up to 100 ms for a
    // host that is plugged in but not reading, and loop() -- which also feeds
    // I2S -- waits with it: an S3 whose port nobody read overflowed its buffer
    // 9 times in a minute and dropped 472 frames (2026-09-30). The buffer holds
    // every line while a host reads; the timeout is the longest a write may
    // then wait before the core stops waiting for that host. Not 0: the core
    // decrements the timeout before testing it, and 0 wraps to forever.
    Serial.setTxBufferSize(USB_SERIAL_TX_BUFFER);
    Serial.setTxTimeoutMs(USB_SERIAL_TX_TIMEOUT_MS);
#endif
    DEBUG_SERIAL.begin(DEBUG_BAUD_RATE);
    delay(1000);

    // Before the banner, because the banner reports it, and before
    // setupESPNow() and any decision about Bluetooth, because both depend on it.
    // Read-only handle — the only writer is the 'c' command.
    prefs.begin(PREF_NAMESPACE, true);
    clientOnly = prefs.getBool(PREF_CLIENT_ONLY, false);
    clientMonoOut = prefs.getBool(PREF_MONO_OUT, false);
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
    if (psramFound()) jStorage = (uint8_t *)ps_malloc(JITTER_BUF_SIZE);
    if (!jStorage)    jStorage = (uint8_t *)malloc(JITTER_BUF_SIZE);
    if (!jbuf.init(jStorage, JITTER_BUF_SIZE)) {
        // A client with no ring drops every packet as an overflow: silent, and
        // `ovf` on the status line says why.
        LOG_ERROR("Jitter buffer init failed — no memory for " + String(JITTER_BUF_SIZE) +
                  " bytes, or not a power of two");
    }
    txEncoder.begin(MESH_BLOCK_FRAMES);

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
                    playJingleOverA2DP(playConnectedSound);
                }
                if (!btConnected) {
                    if (doDisconnectSound) {
                        doDisconnectSound = false;
                        playJingleOverA2DP(playDisconnectedSound);
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
                     " rec=" + String(rxRecovered) +
                     " fgn=" + String(rxForeign));
        } else if (currentMode == MODE_SERVER) {
            LOG_INFO(String("Status: mode=") + modeStr +
                     " heap=" + String(ESP.getFreeHeap()) +
                     " maxalloc=" + String(ESP.getMaxAllocHeap()) +
                     " tx=" + String(txSent) +
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
