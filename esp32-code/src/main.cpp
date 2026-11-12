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
// Update mode only, on a boot that never starts the mesh. See D15.
#include <esp_http_server.h>
#include <esp_ota_ops.h>
// The two-chip server's link (D18).
#include <driver/uart.h>
// The setup network (D19): its captive DNS, and its page.
#include <lwip/sockets.h>
#include "setup_page.h"

// Hardware-independent parts of the CLIENT receive path, tested on the host
// with `pio test -e native` — see test/test_jitter.
#include "adpcm.h"
#include "button.h"
#include "blocks.h"
#include "conceal.h"
#include "gain.h"
#include "drift.h"
#include "holes.h"
#include "jitter.h"
#include "link.h"
#include "losstrace.h"
#include "mesh.h"
#include "seqtracker.h"
#include "sync.h"
#include "tones.h"

#ifdef ENABLE_BLUETOOTH
#include "BluetoothA2DPSink.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_coexist.h>
#endif

// ============================================================================
// Console
// ============================================================================
//
// DEBUG_SERIAL is this: the serial port, plus -- while a command another node
// sent over the mesh is running -- a copy of what that command prints, which
// goes back to the asking node as its reply (D16). Capturing what a command
// printed, rather than giving each command a second way to report, is what
// lets every command work over the mesh without being written twice, and any
// command added later with it.
//
// Only the task that runs the command is captured: a log line the Bluetooth
// or WiFi task prints in the meantime is not part of the answer. The buffer is
// the caller's, on its stack, so capturing costs nothing between commands.
class Console : public Print {
public:
    void begin(unsigned long baud) { Serial.begin(baud); }
    int  available() { return Serial.available(); }
    int  read() { return Serial.read(); }
    void flush() override { Serial.flush(); }

    using Print::write;
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *buf, size_t n) override {
        if (capBuf != nullptr && xTaskGetCurrentTaskHandle() == capTask) {
            const size_t room = capSize - 1 - capLen;
            const size_t k    = n < room ? n : room;
            memcpy(capBuf + capLen, buf, k);
            capLen += k;
            capBuf[capLen] = '\0';
            if (capQuiet) return n;
        }
        return Serial.write(buf, n);
    }

    /** A capture in progress, put aside while another runs inside it. */
    struct Saved {
        char        *buf;
        TaskHandle_t task;
        size_t       size, len;
        bool         quiet;
    };

    /**
     * Copy what this task prints into `buf` from now on; `quiet` keeps it off
     * the port. Returns the capture this one interrupts, for endCapture() to
     * resume: the setup page captures a question to the mesh across several
     * passes of loop(), and a command another node sends in the meantime is
     * captured inside it (D19).
     */
    Saved capture(char *buf, size_t size, bool quiet) {
        const Saved prev = {capBuf, capTask, capSize, capLen, capQuiet};
        buf[0]   = '\0';
        capLen   = 0;
        capSize  = size;
        capQuiet = quiet;
        capTask  = xTaskGetCurrentTaskHandle();
        capBuf   = buf;
        return prev;
    }

    /** Stop capturing, and resume `prev`. @return the bytes captured, terminated in the buffer. */
    size_t endCapture(const Saved &prev) {
        const size_t n = capLen;
        capSize  = prev.size;
        capLen   = prev.len;
        capQuiet = prev.quiet;
        capTask  = prev.task;
        capBuf   = prev.buf;
        return n;
    }

private:
    char *volatile capBuf  = nullptr;
    TaskHandle_t   capTask = nullptr;
    size_t         capSize = 0;
    size_t         capLen  = 0;
    bool           capQuiet = false;
};
static Console debugOut;

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

// ============================================================================
// Front-end mode (D18)
// ============================================================================
//
// A classic board that runs Bluetooth for another node and nothing else: no
// WiFi, no mesh, no speaker. Each A2DP packet goes to the node over the link,
// and the node is the server. A setting like client-only mode, so in NVS, set
// with `F`; never in bench mode, which a front end boots as an ordinary node.
static bool frontEnd = false;

// This node's name: the Bluetooth name, and what `@` and the setup page know it
// by. ROOM_NAME from the build until it is given one of its own (`I<name>`,
// D19), which is kept in NVS -- what D8 said would happen once nodes had a
// configuration interface.
static char roomName[MESH_TARGET_MAX + 1] = ROOM_NAME;

// This node's own speaker, and nothing else. Neither setting touches what is
// received, forwarded, buffered or corrected -- only the samples handed to I2S.
//
// `m` mutes, until reboot: zeroes where this node's samples meet its speaker
// -- on a Bluetooth node after the mesh has taken its copy, on a client before
// I2S. Volume 0 for this node alone, its jingles and beeps included
// (playDrops). It is how a microphone next to one node hears that node alone,
// and how a bench run stays silent.
static volatile bool outputMuted   = false;
// `M` mixes a client's stereo to mono on both channels, kept in NVS: a node
// with one speaker -- a MAX98357A plays one channel -- would otherwise lose
// whatever was panned to the other.
static bool          clientMonoOut = false;
// `v<dB>`: this node's own volume trim, kept in NVS, applied where mute is --
// after the mesh has its copy -- so the phone's slider still moves every room
// together and the trim sets where this room sits among them. For a loud amp
// on one node (WROVER2's TPA3116) and a small one on another.
static int8_t        outTrimDb     = 0;
static volatile int32_t outTrimQ12 = GAIN_UNITY_Q12;

static const char *PREF_NAMESPACE = "sonoloco";
static const char *PREF_CLIENT_ONLY = "clientonly";
static const char *PREF_FRONT_END = "frontend";
static const char *PREF_ROOM_NAME = "roomname";
static const char *PREF_AP_PASS = "appass";   // the setup network's (D19)
static const char *PREF_MONO_OUT = "monoout";
static const char *PREF_TRIM_DB = "trimdb";
static const char *PREF_MESH_ID = "meshid";
static const char *PREF_MESH_NAME = "meshname";
static const char *PREF_WIFI_SSID = "wifissid";   // update mode's home network
static const char *PREF_WIFI_PASS = "wifipass";

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

/** A phone reaches this node through this chip's own Bluetooth. */
static inline bool ownBt() {
#ifdef ENABLE_BLUETOOTH
    return btAllowed() && !frontEnd;
#else
    return false;
#endif
}

/**
 * A phone reaches this node through a front end on the link instead (D18). Not
 * beside its own Bluetooth -- a classic node takes a front end only in
 * client-only mode, or a phone would see two speakers -- and not in bench
 * mode, where SERVER is the node's own tone.
 */
static inline bool linkSourceAllowed() {
    return !benchMode && !frontEnd && !ownBt();
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

// A short press opened Bluetooth on a node that keeps it off (D17), until
// btWindowUntilMs or until a phone connects. While it is open the node stays
// out of CLIENT, or the mesh's stream would switch Bluetooth straight off.
static bool          btWindow        = false;
static unsigned long btWindowUntilMs = 0;
static bool          btWindowTone    = false;   // the two drops, on the next loop pass
// The setup network is open, and this chip's Bluetooth paused for it (D19).
static bool          cfgOpen         = false;
static bool          cfgPausedBt     = false;
static unsigned long lastBeaconMs    = 0;

// Packets dropped because they belong to somebody else's mesh. Worth counting
// rather than ignoring: it is the difference between "the neighbours are
// audible but correctly ignored" and "nothing is arriving at all", which look
// identical from every other number this firmware reports.
static volatile uint32_t rxForeign = 0;

// An update request named this node. Set by the receive callback and acted on
// in loop(), which reboots -- for the same reason pairing is committed there.
static volatile bool otaRequested = false;
// This node's MAC as AA:BB:CC:DD:EE:FF, the other name a request may use.
static char          ownMac[18]   = {0};

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

// Frames written to the output driver since it was installed -- tones, silence
// and audio alike, because the driver fills its DMA buffers in exactly the
// order they were written, and the output clock (lib/sync) reads its position
// from this count. Reset by initI2SOutput().
static uint32_t     outFrames = 0;
static PlayoutClock outClock;

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
    outFrames += done / 4;
    return done;
}

// ============================================================================
// Notification sounds (startup / connect / disconnect)
// ============================================================================
//
// Water drops through an echo, made by lib/tones and chosen by ear in
// tools/tones/preview.html (2026-10-06). They replaced beeps, and keep what
// the beeps meant: two at one pitch, three rising, three falling (D12).

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

static const DropSound::Timbre DROP_TIMBRE = {
    DROP_GLIDE_DEPTH_ST, DROP_GLIDE_MS, DROP_DECAY_MS, DROP_GLASS, DROP_GLASS_RATIO, DROP_GLASS_MS,
    DROP_ECHO_GLASS_KEEP, DROP_ECHO_MS, DROP_ECHO_LEVEL, DROP_ECHO_REPEATS,
};

/**
 * Play drops into the output driver as they are rendered, then 50 ms of
 * silence, so the DMA has played the end out before a caller uninstalls it.
 *
 * Rendering is integer arithmetic a sample (lib/tones), and has to be: the C3
 * has no FPU, and when its beeps were a sinf() a sample it computed them at a
 * sixth of the speed they play. The DMA, cleared whenever it ran dry
 * (tx_desc_auto_clear), played them as bursts with silence between: 1,410 ms
 * for 330 ms of sound (2026-10-03). test_tones times the render on the board.
 *
 * A muted node plays its jingles and beeps as silence, for exactly as long:
 * what follows a sound (the schedule a jingle invalidates) runs unchanged.
 * Before, a muted server played its connect jingle as the PC dialled in,
 * during a run meant to be silent (2026-10-03).
 */
static void playDrops(const DropSound::Note *notes, int count) {
    DropSound     sound;
    int16_t       mono[256];
    int16_t       stereo[256 * 2];
    const int32_t amp = outputMuted ? 0 : TONE_AMPLITUDE;
    int           n;

    sound.begin(notes, count, DROP_TIMBRE, BT_SAMPLE_RATE);
    while ((n = sound.render(mono, 256, amp)) > 0) {
        for (int i = 0; i < n; i++) stereo[i * 2] = stereo[i * 2 + 1] = mono[i];
        size_t bw = i2sWriteAll(stereo, n * 4, pdMS_TO_TICKS(100));
        if (bw < (size_t)n * 4) return;   // I2S is not accepting — abort the sound
    }
    playSilence(50);
}

// Drops at 0.8 leave room for the echoes that overlap them: each sound peaks
// within a few percent of TONE_AMPLITUDE, where the beeps did.

void playStartupSound() {         // listening: two drops at one pitch
    static const DropSound::Note notes[] = {
        {0,           DROP_NOTE_MID, 0.8, 1.0},
        {DROP_GAP_MS, DROP_NOTE_MID, 0.8, 1.4},
    };
    playDrops(notes, sizeof(notes) / sizeof(notes[0]));
}

void playConnectedSound() {       // joined: three rising
    static const DropSound::Note notes[] = {
        {0,               DROP_NOTE_LOW,  0.8, 1.0},
        {DROP_GAP_MS,     DROP_NOTE_MID,  0.8, 1.0},
        {2 * DROP_GAP_MS, DROP_NOTE_HIGH, 0.8, 1.8},
    };
    playDrops(notes, sizeof(notes) / sizeof(notes[0]));
}

void playDisconnectedSound() {    // nothing heard: three falling
    static const DropSound::Note notes[] = {
        {0,               DROP_NOTE_HIGH, 0.8, 1.0},
        {DROP_GAP_MS,     DROP_NOTE_MID,  0.8, 1.0},
        {2 * DROP_GAP_MS, DROP_NOTE_LOW,  0.8, 1.8},
    };
    playDrops(notes, sizeof(notes) / sizeof(notes[0]));
}

void playTickSound() {            // a held button has reached the next range: one drop
    static const DropSound::Note notes[] = {
        {0, DROP_NOTE_HIGH, 0.6, 1.0},
    };
    playDrops(notes, sizeof(notes) / sizeof(notes[0]));
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

#define ESPNOW_HEADER_SIZE 8   // group (2) + seq (2) + len (2) + due (2)

// `group` is first because it is the field that decides whether the rest is
// even ours to look at -- a receiver rejects a neighbour's packet having read
// two bytes.
//
// The payload is MESH_BLOCKS_PER_PACKET ADPCM blocks (lib/adpcm): the block
// numbered `seq`, then redundancy for older ones (D13) -- either the block
// numbered `seq - distance` itself, or, with ESPNOW_LEN_XOR, the XOR of blocks
// `seq - 1` and `seq - distance`. `len` says how many bytes are there -- the
// first packets of a stream have nothing older to carry -- and, in its upper
// bits, the distance, the mode and whether this is a repeat.
//
// `due` is when block `seq` plays on the sender's own speaker, counted from the
// moment this frame was handed to the radio, in MESH_DUE_UNIT_US steps; stamped
// in the transmit task, so a repeat copy carries its own. A client adds it to
// the time it received the frame and has the sender's play time on its own
// clock -- see lib/sync. MESH_DUE_NONE while the sender is not playing on a
// schedule yet (its own output still filling), and on beacons.
typedef struct __attribute__((packed)) {
    uint16_t group;
    uint16_t seq;
    uint16_t len;
    uint16_t due;
    uint8_t  data[ESPNOW_PAYLOAD_SIZE];
} AudioPacket;  // 248 bytes total, under the 250-byte ESP-NOW limit

#define MESH_DUE_NONE  0xFFFF

static_assert(ESPNOW_HEADER_SIZE + ESPNOW_PAYLOAD_SIZE <= 250,
              "ESP-NOW cannot send more than 250 bytes per frame");
static_assert(MESH_BLOCK_BYTES == adpcmBlockBytes(MESH_BLOCK_FRAMES),
              "MESH_BLOCK_BYTES must match lib/adpcm's block layout");
static_assert(MESH_BLOCK_FRAMES <= ADPCM_MAX_FRAMES, "block too long for the encoder");

// `len` packs three fields; a payload is at most 244 bytes, so its low byte
// holds the length and the rest is free.
//   bits 0-7   payload bytes. Mask with ESPNOW_LEN_BYTES before using `len`
//              as a length anywhere -- unmasked it is tens of kilobytes.
//   bits 8-11  the distance: the second block is `seq - distance`, or its
//              parity with `seq - 1`. On the wire so a server can change it
//              (`D<n>`, `X<n>`) and every client follows without a reflash.
//   bit 12     ESPNOW_LEN_XOR: the second block is that parity.
//   bit 15     set on the second and later copies of a frame (ESPNOW_TX_COPIES).
//              The receiver needs it only for its counters: a copy whose
//              original arrived is dropped either way, but it was meant, and
//              must not read as a duplicate nobody sent.
#define ESPNOW_LEN_BYTES       0x00FF
#define ESPNOW_LEN_DIST_SHIFT  8
#define ESPNOW_LEN_DIST_MAX    0x0F
#define ESPNOW_LEN_XOR         0x1000
#define ESPNOW_LEN_REPEAT      0x8000
static_assert(ESPNOW_PAYLOAD_SIZE <= ESPNOW_LEN_BYTES, "payload length must fit len's low byte");
static_assert(MESH_TX_HISTORY <= ESPNOW_LEN_DIST_MAX + 1, "distance must fit len's bits 8-11");
static_assert(MESH_TX_HISTORY <= HoleTable::SLOTS && MESH_TX_HISTORY <= BlockStore::SLOTS,
              "clients must remember as far back as servers reach");
static_assert((MESH_TX_HISTORY & (MESH_TX_HISTORY - 1)) == 0, "MESH_TX_HISTORY must be a power of two");
static_assert(MESH_REDUNDANCY_DISTANCE < MESH_TX_HISTORY, "the distance must be inside the history");

// XORed into the mesh id on the wire. A node on another audio format sees this
// format's packets as somebody else's mesh and drops them, and the other way
// round -- which matters, because the alternative is worse than silence: a
// node from before ADPCM would play these bytes as raw PCM, full-scale noise
// through whatever amp it has. `fgn=` climbing on a node that should be
// playing is the sign of a node left on the old firmware. Change it whenever
// the payload changes meaning. XOR is its own inverse, so one helper both
// stamps and reads. 0xAD01 was ADPCM with the previous block; 0xAD02 carried
// the distance in `len`; 0xAD03 added the parity mode, which a 0xAD02 client
// would play as audio; 0xAD04 adds `due`, which moves the payload two bytes.
#define MESH_WIRE_FORMAT   0xAD04
static inline uint16_t wireGroup(uint16_t id) { return id ^ MESH_WIRE_FORMAT; }

// The group control packets travel in -- today only the update request, see
// MESH_UPDATE_SEQ. Not the audio group, on purpose: a node from before update
// mode drops this as another mesh's traffic, where in the audio group it would
// lock onto the sender before checking the length and then ignore its server.
#define MESH_CONTROL_FORMAT 0xC7D1
static_assert(MESH_CONTROL_FORMAT != MESH_WIRE_FORMAT, "control must not look like audio");

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- Playing in time: when this node plays what (lib/sync) ---
//
// Written by whatever drives this node's speaker -- the ring's reader in
// loop(), or the bench source's own clock -- and read by the transmit task,
// which turns it into each packet's `due`. A frame index and a time that must
// be read as a pair, and two tasks on two cores: hence the lock rather than a
// pair of volatiles.
//
// The frame index is in ring numbering (ringPushed below): on a server, the
// frames of the A2DP stream as they went into its own ring; on a bench source,
// the frames it has generated.
static portMUX_TYPE schedMux   = portMUX_INITIALIZER_UNLOCKED;
static bool         schedValid = false;
static uint32_t     schedFrame = 0;
static uint32_t     schedUs    = 0;

static void schedPublish(uint32_t frame, uint32_t us) {
    portENTER_CRITICAL(&schedMux);
    schedFrame = frame;
    schedUs    = us;
    schedValid = true;
    portEXIT_CRITICAL(&schedMux);
}

static void schedInvalidate() {
    portENTER_CRITICAL(&schedMux);
    schedValid = false;
    portEXIT_CRITICAL(&schedMux);
}

/** When ring frame `frame` plays on this node, if it is playing on a schedule. */
static bool schedPlayUs(uint32_t frame, uint32_t *us) {
    portENTER_CRITICAL(&schedMux);
    const bool     ok = schedValid;
    const uint32_t f0 = schedFrame, t0 = schedUs;
    portEXIT_CRITICAL(&schedMux);
    if (ok) *us = t0 + (uint32_t)syncFramesToUs((int32_t)(frame - f0), CLIENT_SAMPLE_RATE);
    return ok;
}

// --- TX (SERVER mode) ---
// A queued packet, and the ring frame its block starts at: `due` is worked out
// from that as the packet leaves, not when it was queued, because only then is
// the time it spent waiting for the radio known.
typedef struct {
    AudioPacket pkt;
    uint32_t    first;
} TxItem;

static bool              espnowActive = false;
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
// each completed block goes out with redundancy for older ones behind it:
// the block `txDistance` back, or with `txParity` the XOR of that one and the
// one just before (MESH_REDUNDANCY_*; `D<n>` and `X<n>` change them until
// reboot).
static AdpcmStereoEncoder txEncoder;
static uint8_t            txHistory[MESH_TX_HISTORY][MESH_BLOCK_BYTES];   // by seq
static uint16_t           txHistoryBlocks = 0;   // blocks since meshTxReset, saturating
static volatile uint8_t   txDistance      = MESH_REDUNDANCY_DISTANCE;
static volatile bool      txParity        = MESH_REDUNDANCY_PARITY;
// The ring frame the block being encoded starts at, and how far into it we are.
static uint32_t           txBlockFirst    = 0;
static uint32_t           txBlockFrames   = 0;

/** Start a new stream: no state carried over, and no older block to send. */
static void meshTxReset() {
    txEncoder.reset();
    txHistoryBlocks = 0;
    txBlockFrames   = 0;
}

/**
 * Add one stereo frame to the outgoing stream; `frame` is its index in this
 * node's ring numbering (see schedPlayUs). Returns true when it completed a
 * packet, which has then been queued (or counted as `qfull`).
 */
static bool meshTxFrame(int16_t left, int16_t right, uint32_t frame) {
    if (txBlockFrames++ == 0) txBlockFirst = frame;
    if (!txEncoder.push(left, right)) return false;
    txBlockFrames = 0;

    TxItem item;
    item.first = txBlockFirst;
    AudioPacket &pkt = item.pkt;
    pkt.group = wireGroup(meshId);
    pkt.seq   = txSeq++;
    memcpy(pkt.data, txEncoder.block(), MESH_BLOCK_BYTES);
    uint16_t len = MESH_BLOCK_BYTES;
    const uint8_t d      = txDistance;
    const bool    parity = txParity && d >= 2;   // at 1 the parity would be zeroes
    if (d > 0 && txHistoryBlocks >= d) {
        const uint8_t *far = txHistory[(uint16_t)(pkt.seq - d) & (MESH_TX_HISTORY - 1)];
        if (parity) {
            xorBlocks(pkt.data + MESH_BLOCK_BYTES,
                      txHistory[(uint16_t)(pkt.seq - 1) & (MESH_TX_HISTORY - 1)], far,
                      MESH_BLOCK_BYTES);
        } else {
            memcpy(pkt.data + MESH_BLOCK_BYTES, far, MESH_BLOCK_BYTES);
        }
        len += MESH_BLOCK_BYTES;
    }
    pkt.len = len | (uint16_t)(d << ESPNOW_LEN_DIST_SHIFT) | (parity ? ESPNOW_LEN_XOR : 0);
    memcpy(txHistory[pkt.seq & (MESH_TX_HISTORY - 1)], txEncoder.block(), MESH_BLOCK_BYTES);
    if (txHistoryBlocks < 0xFFFF) txHistoryBlocks++;

    if (xQueueSend(txQueue, &item, 0) != pdTRUE) txQueueFull++;
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

// The ring's two ends in frames, counted since boot and never reset, so that a
// frame keeps one index for its whole life in the ring: its ringPushed at the
// moment it went in. ringPushed is advanced only by the producer (the receive
// callback, or a server's A2DP callback), ringConsumed only by loop() --
// played, dropped, or discarded all count. The schedule (schedPlayUs) and the
// server's timeline are both in this numbering.
static volatile uint32_t ringPushed   = 0;
static uint32_t          ringConsumed = 0;

/** Empty the ring. The producer must not be running, as for jbuf.reset(). */
static void ringReset() {
    jbuf.reset();
    ringConsumed = ringPushed;
}

// The server's schedule as the stamps describe it (lib/sync): add() from the
// receive callback, harvest() from loop(), one lock between them.
static portMUX_TYPE      timelineMux   = portMUX_INITIALIZER_UNLOCKED;
static ServerTimeline    timeline;
// Set by the receive callback when the ring's numbering stops matching the
// stream -- a resync, a gap longer than the silence it was given, an overflow
// -- so the next harvest forgets the line instead of steering by it.
static volatile bool     timelineFlush = false;
static volatile uint32_t rxStampedMs   = 0;   // the last packet carrying a `due`
static SeqTracker   seqTracker(SEQ_RESYNC_THRESHOLD, MAX_GAP_FILL_PKTS);
// Where each lost block's silence went, for the later packet that carries it,
// and the encoded blocks held, for undoing a parity. Both keyed by seq:
// cleared with jbuf, and on a resync, which renumbers.
static HoleTable    rxHoles;
static uint8_t      rxStoreBuf[BlockStore::SLOTS * MESH_BLOCK_BYTES];
static BlockStore   rxStore;
// Concealment (MESH_CONCEAL, `z`): the last block pushed, decoded, which the
// next gap fades out of. Invalid at a stream's start and after a resync.
static volatile bool rxConceal   = MESH_CONCEAL;
static int16_t       rxPrev[2 * MESH_BLOCK_FRAMES];
static bool          rxPrevValid = false;

// Clock-drift correction. The controller only decides; driveRingI2S applies.
// Runtime-switchable rather than compile-time so a bench run can measure the
// same boards with it on and off -- the before/after is the only evidence that
// it does anything, and D9's reasoning applies: test the binary that ships.
static DriftController driftCtl;
// The same, on a Bluetooth server's own ring, which is fed a packet at a time
// and so runs lower (SERVER_TARGET_BYTES). levelCtl() is whichever applies.
static DriftController srvCtl;
static bool            driftEnabled = true;

static volatile bool jReady   = false;   // true once prefill threshold is met

// Playing on the server's schedule (lib/sync): steering on the error between
// when this node plays a frame and when the server does, rather than on the
// ring's depth. syncCtl is the drift controller with a fixed target, handed
// that error as if it were a fill; driftCtl stays the level controller for a
// server, and for a client whose server sends no stamps.
static bool            outSynced  = false;
static DriftController syncCtl;
static volatile int32_t syncErrUs = 0;   // the last error, + = this node late (`se=`)
static volatile uint32_t syncJumps = 0;  // realignments beyond SYNC_JUMP_US (`sjmp=`)
static volatile uint32_t outDry    = 0;  // the DMA ran dry under a playing ring (`dry=`)

// syncCtl works in bytes of fill, like driftCtl. Its target is this, fixed,
// and a timing error is handed to it as a fill that much above or below:
// any positive number clear of the error's range will do.
static const int SYNC_FILL_BASE = 65536;
static inline int syncUsToBytes(int32_t us) {
    return (int)((int64_t)us * (CLIENT_SAMPLE_RATE * CLIENT_FRAME_BYTES) / 1000000LL);
}

/** The level controller for this node's ring: a server's own, or a client's. */
static inline DriftController &levelCtl() {
    return currentMode == MODE_SERVER ? srvCtl : driftCtl;
}
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
// Blocks whose original frame was lost and that played anyway: rebuilt from a
// later packet's redundancy, or a repeat copy standing in. `rec` on the
// status lines.
static volatile uint32_t rxRecovered = 0;
// Blocks rebuilt too late: their silence had already played, or was about to.
// A count that grows with the distance says the buffer is too shallow for it.
// `late=` in `l`, which resets it.
static volatile uint32_t rxLate = 0;

// The outcome of every packet, lost or heard, for scoring redundancy schemes
// offline on one recording (lib/jitter/losstrace.h, tools/btlisten/losstrace.py).
// Off until `L1`; loop() prints what accumulated once a second as `[LT]`
// lines, about 120 bytes/s at 387 pkt/s. add() runs in the receive callback,
// take() in loop(), hence the lock.
static portMUX_TYPE      traceMux = portMUX_INITIALIZER_UNLOCKED;
static LossTrace         rxTrace;
static volatile bool     traceOn  = false;

// Lock onto the first node we hear so two simultaneous servers can never
// interleave their streams into one jitter buffer.
static uint8_t          lockedSender[6] = {0};
static volatile bool    senderLocked    = false;

/**
 * How long from now until ring frame `first` plays on this node, in the units
 * `due` travels in -- or MESH_DUE_NONE if this node is not playing on a
 * schedule, or the frame has already played (a packet that waited that long
 * for the radio says nothing a client could use).
 */
static uint16_t meshDue(uint32_t first) {
    uint32_t playUs;
    if (!schedPlayUs(first, &playUs)) return MESH_DUE_NONE;
    const int32_t ahead = (int32_t)(playUs - (uint32_t)micros());
    if (ahead < 0 || ahead >= (int32_t)MESH_DUE_NONE * MESH_DUE_UNIT_US) return MESH_DUE_NONE;
    return (uint16_t)(ahead / MESH_DUE_UNIT_US);
}

static void espnowTxTask(void *) {
    TxItem item;
    AudioPacket &pkt = item.pkt;
    uint32_t lastAudioUs = 0;
    while (true) {
        if (xQueueReceive(txQueue, &item, portMAX_DELAY) != pdTRUE) continue;

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

            // The last moment before the radio: whatever the frame waits from
            // here on is transit, which the client's minimum takes out.
            pkt.due = pkt.len ? meshDue(item.first) : MESH_DUE_NONE;
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

// ============================================================================
// Control traffic: update requests, commands and their replies
// ============================================================================
//
// Everything in the control group (MESH_CONTROL_FORMAT): the update request of
// D15 and the commands of D16, which are any serial command addressed to
// another node. One way out and one way in for all of it.
//
// Out: a request goes as broadcast, which nothing acknowledges, so it is
// repeated. One at a time per node -- a new one replaces whatever is still
// repeating -- through the same queue as audio, so a server can send while it
// plays.
static struct {
    uint16_t      seq;
    uint8_t       payload[3 + MESH_TARGET_MAX + MESH_COMMAND_MAX];
    uint8_t       len;
    uint8_t       left;
    uint16_t      intervalMs;
    unsigned long lastMs;
} ctrlOut = {};

static void ctrlSend(uint16_t seq, const uint8_t *payload, size_t len, uint8_t copies,
                     uint16_t intervalMs) {
    ctrlOut.seq        = seq;
    ctrlOut.len        = (uint8_t)len;
    ctrlOut.left       = copies;
    ctrlOut.intervalMs = intervalMs;
    ctrlOut.lastMs     = 0;
    memcpy(ctrlOut.payload, payload, len);
}

/** One control packet into the transmit queue. Never waits: a full queue is a server mid-stream. */
static bool ctrlQueue(uint16_t seq, const uint8_t *payload, size_t len) {
    if (!espnowActive || txQueue == nullptr) return false;
    TxItem item;
    item.first     = 0;
    item.pkt.group = (uint16_t)(meshId ^ MESH_CONTROL_FORMAT);
    item.pkt.seq   = seq;
    item.pkt.len   = (uint16_t)len;
    item.pkt.due   = MESH_DUE_NONE;
    memcpy(item.pkt.data, payload, len);
    if (xQueueSend(txQueue, &item, 0) != pdTRUE) {
        txQueueFull++;
        return false;
    }
    return true;
}

// In: the receive callback only records, as it does for pairing -- loop()
// runs the command, which may print, write NVS or reboot.
//
// A command addressed to this node, waiting for loop(). One slot: while it is
// full, more commands are dropped, and the asking node's next copy tries again.
static struct {
    volatile bool ready;
    uint16_t      id;
    uint8_t       from[6];
    unsigned long runAtMs;
    char          text[MESH_COMMAND_MAX + 1];
} ctrlIn = {};
// The last command taken, so the other copies of it are not run again: `m`
// toggles, and five copies of it would leave the speaker muted.
static uint16_t ctrlLastId      = 0;
static uint8_t  ctrlLastFrom[6] = {0};

// Its answer, kept while the asking node's copies of that command are still
// arriving: each later copy says the answer may not have got through, and it
// goes again. An answer sent once was lost 2 times in 10 while a phone was
// paging the asking WROVER (2026-10-02), and the asking node is often the one
// a phone streams to. On the heap, for half a second, after a command only.
static struct {
    char                   *text;
    size_t                  len;
    volatile uint16_t       id;
    unsigned long           untilMs;
    volatile bool           resend;
    volatile unsigned long  resendAtMs;
} ctrlLast = {};

// The asking side. The replies to this node's own command, heard in the
// callback and printed by loop(): a reply is up to a packet of text, and the
// callback must not print. The queue is made the first time this node asks,
// so a node nobody types `@` into never pays for it.
typedef struct {
    uint8_t len;
    uint8_t data[ESPNOW_PAYLOAD_SIZE];
} ReplyItem;
static QueueHandle_t          replyQueue    = nullptr;
static volatile uint16_t      cmdAskId      = 0;   // 0: not waiting for any
static volatile unsigned long cmdAskUntilMs = 0;
static uint16_t               cmdAskReplies = 0;
// The parts already printed, by name and part number, so an answer sent again
// (ctrlLast) is printed once.
static uint32_t               cmdAskSeen[16];
static uint8_t                cmdAskSeenCount = 0;
// The question being waited on, to ask again if nobody answers it. Only a
// question to one node, and only one that means the same asked twice.
static char                   cmdAskLine[2 + MESH_TARGET_MAX + MESH_COMMAND_MAX];
static bool                   cmdAskAll     = false;
static uint8_t                cmdAskRetries = 0;

/** A packet in this mesh's control group. Called from the receive callback, in every mode. */
static void ctrlReceive(const uint8_t *srcMac, const uint8_t *data, int len) {
    uint16_t seq, plen;
    memcpy(&seq,  data + 2, 2);
    memcpy(&plen, data + 4, 2);
    const bool repeat = (plen & ESPNOW_LEN_REPEAT) != 0;
    plen &= ESPNOW_LEN_BYTES;
    if ((int)plen > len - ESPNOW_HEADER_SIZE) return;
    const uint8_t *payload = data + ESPNOW_HEADER_SIZE;

    if (seq == MESH_UPDATE_SEQ) {
        // Ignored while serving a phone: every room's stream would stop.
        if (currentMode != MODE_SERVER && meshIsUpdateRequest(seq, plen) &&
            meshTargetMatches((const char *)payload, plen, roomName, ownMac)) {
            otaRequested = true;
        }
        return;
    }

    if (seq == MESH_COMMAND_SEQ) {
        MeshCommand cmd;
        if (ctrlIn.ready || !meshUnpackCommand(payload, plen, &cmd)) return;
        if (!meshCommandAddressed(cmd.target, cmd.targetLen, cmd.text[0], roomName, ownMac)) return;
        // A command for everyone gets everyone's answer at once, and
        // broadcasts are not retried: each node waits its own random while,
        // to answer and to answer again.
        const bool all = cmd.targetLen == 1 && cmd.target[0] == MESH_TARGET_ALL[0];
        if (cmd.id == ctrlLastId && memcmp(srcMac, ctrlLastFrom, 6) == 0) {
            if (ctrlLast.text != nullptr && ctrlLast.id == cmd.id) {
                ctrlLast.resendAtMs = millis() + (all ? esp_random() % (MESH_COMMAND_INTERVAL_MS / 2) : 0);
                ctrlLast.resend     = true;
            }
            return;
        }
        ctrlLastId = cmd.id;
        memcpy(ctrlLastFrom, srcMac, 6);

        ctrlIn.id = cmd.id;
        memcpy(ctrlIn.from, srcMac, 6);
        memcpy(ctrlIn.text, cmd.text, cmd.textLen);
        ctrlIn.text[cmd.textLen] = '\0';
        ctrlIn.runAtMs = millis() + (all ? esp_random() % MESH_REPLY_SPREAD_MS : 0);
        ctrlIn.ready   = true;
        return;
    }

    if (seq == MESH_REPLY_SEQ) {
        // A repeat copy (`t2`, `t3`) adds nothing to text already printed.
        if (repeat || replyQueue == nullptr || cmdAskId == 0 || plen < 2) return;
        if ((uint16_t)(payload[0] | (payload[1] << 8)) != cmdAskId) return;
        ReplyItem item;
        item.len = (uint8_t)plen;
        memcpy(item.data, payload, plen);
        xQueueSend(replyQueue, &item, 0);
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    const uint8_t *srcMac = info->src_addr;
#else
static void onEspNowRecv(const uint8_t *srcMac, const uint8_t *data, int len) {
#endif
    // First, before anything can delay it: `due` counts from the send, and
    // everything between the air and this line is added to the transit.
    const uint32_t rxUs = micros();
    if (len < ESPNOW_HEADER_SIZE) return;

    // The received buffer has no alignment guarantee; copy the header out
    // instead of casting to a struct pointer.
    uint16_t group;
    memcpy(&group, data, 2);

    // This mesh's control traffic: update requests, commands, replies. Heard in
    // every mode -- a server is a node like any other to a command, and the one
    // a listing most needs to hear from -- so before the server's return below.
    // Before the foreign test, which would count it, and before the sender
    // lock, which it must never take.
    if ((uint16_t)(group ^ MESH_CONTROL_FORMAT) == meshId) {
        ctrlReceive(srcMac, data, len);
        return;
    }

    // A SERVER is the source, not a listener. Bailing before the lock below
    // matters: locking here would pin lockedSender to whatever stray node we
    // happened to hear while serving, and that lock outlives SERVER mode.
    if (currentMode == MODE_SERVER) return;

    uint16_t seq, plen, due;
    memcpy(&seq,   data + 2, 2);
    memcpy(&plen,  data + 4, 2);
    memcpy(&due,   data + 6, 2);
    const bool     repeat = (plen & ESPNOW_LEN_REPEAT) != 0;
    const bool     parity = (plen & ESPNOW_LEN_XOR) != 0;
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

    // A stream that paused and resumed carries on in sequence, but the server
    // re-armed its own output in between: the old line says nothing now.
    const unsigned long nowMs = millis();
    if (lastRxMs && (long)(nowMs - lastRxMs) > (long)SYNC_STREAM_GAP_MS) timelineFlush = true;
    lastRxMs = nowMs;

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
    if (seqTracker.resync != resyncBefore) {   // numbering restarted
        rxHoles.clear();
        rxStore.clear();
        rxPrevValid   = false;
        timelineFlush = true;
    }
    // Blocks, not frames: with every frame sent twice, counting frames would
    // make rx twice the source's tx and halve every loss percentage.
    rxCount++;
    if (repeat) rxRecovered++;   // the original was lost and the copy stood in
    const uint32_t run = seqTracker.lost - lostBefore;
    if (run) rxLossRuns[run < 8 ? run - 1 : 7]++;
    if (traceOn) {
        portENTER_CRITICAL(&traceMux);
        rxTrace.add(seq, run);
        portEXIT_CRITICAL(&traceMux);
    }

    // Decoded before the gap is filled: the block after a gap is half of what
    // fills it. Static: this is the WiFi task's stack, and it is not ours to
    // spend.
    static int16_t pcm[2 * MESH_BLOCK_FRAMES];
    static int16_t fill[2 * MESH_BLOCK_FRAMES];
    const bool decoded = adpcmDecodeStereoBlock(payload, MESH_BLOCK_FRAMES, pcm);

    // What was missed goes in, in playing order, so playback keeps its timing
    // instead of splicing the stream shorter on every loss -- as silence, or
    // concealed (lib/jitter/conceal.h) -- and each block's place is
    // remembered, for the packet that carries it later.
    static const int BLOCK_PCM = MESH_BLOCK_FRAMES * CLIENT_FRAME_BYTES;
    const uint32_t ovfBefore = rxOverflow;
    for (int i = sr.fillPackets; i > 0; i--) {
        const int pos = jbuf.writePos();
        bool ok;
        if (rxConceal) {
            concealBlock(rxPrevValid ? rxPrev : nullptr, decoded ? pcm : nullptr,
                         MESH_BLOCK_FRAMES, sr.fillPackets, sr.fillPackets - i, fill);
            ok = jbuf.pushBlock((const uint8_t *)fill, BLOCK_PCM);
        } else {
            ok = jbuf.pushSilence(BLOCK_PCM);
        }
        if (ok) {
            rxHoles.add((uint16_t)(seq - i), pos);
            ringPushed += MESH_BLOCK_FRAMES;
        } else {
            rxOverflow++;
        }
    }
    // A gap longer than the silence it was given: the ring is now shorter than
    // the stream, and the server's timeline in ring frames has moved.
    if (run > (uint32_t)sr.fillPackets) timelineFlush = true;

    const uint32_t first = ringPushed;
    const bool pushed = decoded ? jbuf.pushBlock((const uint8_t *)pcm, BLOCK_PCM)
                                : jbuf.pushSilence(BLOCK_PCM);
    if (pushed) ringPushed += MESH_BLOCK_FRAMES;
    else        rxOverflow++;
    if (rxOverflow != ovfBefore) timelineFlush = true;   // frames missing from the ring
    // What the next gap fades out of.
    if (decoded) memcpy(rxPrev, pcm, sizeof(rxPrev));
    rxPrevValid = decoded;

    // When the server plays this block, on this node's clock -- late by this
    // frame's transit beyond the fastest, which the timeline's minimum removes.
    if (pushed && due != MESH_DUE_NONE) {
        const uint32_t at = rxUs + (uint32_t)due * MESH_DUE_UNIT_US - MESH_TRANSIT_MIN_US;
        portENTER_CRITICAL(&timelineMux);
        timeline.add(first, at);
        portEXIT_CRITICAL(&timelineMux);
        rxStampedMs = millis();
    }

    rxStore.put(seq, payload);

    // The redundancy: an older block itself, or the parity of two, of which
    // the one still missing falls out if the other is held. It goes over the
    // silence played in its place, if that has not played yet; the guard
    // keeps the write clear of the batch the I2S side may be copying out.
    if (blocks < 2 || dist == 0) return;
    static uint8_t rebuilt[MESH_BLOCK_BYTES];
    const uint8_t *second = payload + MESH_BLOCK_BYTES;
    const uint8_t *block  = nullptr;
    uint16_t       target = 0;
    if (!parity) {
        target = (uint16_t)(seq - dist);
        block  = second;
    } else {
        const uint8_t *known;
        if (parityRebuildable(rxStore, seq, dist, &target, &known)) {
            xorBlocks(rebuilt, second, known, MESH_BLOCK_BYTES);
            block = rebuilt;
        }
    }
    int pos;
    if (block == nullptr || !rxHoles.take(target, &pos)) return;
    rxStore.put(target, block);   // it may complete another parity later
    if (adpcmDecodeStereoBlock(block, MESH_BLOCK_FRAMES, pcm) &&
        jbuf.patch(pos, (const uint8_t *)pcm, BLOCK_PCM, CLIENT_BATCH * CLIENT_FRAME_BYTES)) {
        seqTracker.recovered(1);   // played, so not a hole
        rxRecovered++;
        rxCount++;                 // rx + lost still adds up to the source's tx
    } else {
        rxLate++;
    }
}

// Whether this boot meant to run the mesh. False only on a node that skipped it
// on purpose (a WROOM with Bluetooth); with it true and espnowActive false the
// radio failed, which is what keeps a fresh update from being kept (otaServiceConfirm).
static bool espnowWanted = false;

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
    espnowWanted = true;

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
    // (WIFI_PS_MIN_MODEM). It costs a client nothing measurable: modem sleep
    // engages only while associated with an AP, which this mesh never is.
    // WROVER1 as a CLIENT of a bench source, 600 s each way (2026-10-01): 69
    // packets missed in normal mode, 123 in bench mode with PS_NONE, against
    // 254 and 271 on an S3 beside it.
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

    txQueue = xQueueCreate(ESPNOW_TX_QUEUE_DEPTH, sizeof(TxItem));
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
// The link — a two-chip server's halves (D18)
// ============================================================================
//
// A front end runs Bluetooth and nothing else, and hands the phone's audio and
// its state to a node over a UART; the node plays it and forwards it as a
// server. Frames in lib/link, the wiring in config.h. The same code on both
// ends, which end it is decided by frontEnd.

static_assert(LINK_AUDIO_CHUNK * CLIENT_FRAME_BYTES <= LINK_MAX_PAYLOAD,
              "an audio chunk must fit in one link frame");
static_assert(LINK_TONE_PACKET_FRAMES % LINK_AUDIO_CHUNK == 0,
              "the link tone sends whole chunks");
static_assert(MESH_REPLY_CAPTURE <= LINK_MAX_PAYLOAD,
              "a command's answer must fit in one link frame");

// The link's buffers come from the heap in linkBegin(), on the boards that
// start it: as static arrays they took 3.7 KB of internal DRAM on every
// classic board, a WROVER server's included, which never uses them and has
// least of it to spare (D3, D15).
static bool              linkUp       = false;
static SemaphoreHandle_t linkTxLock   = nullptr;   // one frame, one sequence number, at a time
static uint8_t           linkTxSeq    = 0;
static uint8_t          *linkTxBuf    = nullptr;   // LINK_FRAME_MAX
static volatile uint32_t linkTxFrames = 0;
// Audio the front end could not send: the UART's buffer full, or the lock
// held too long. Dropped rather than waited for, because the A2DP callback is
// the Bluetooth stack's own task.
static volatile uint32_t linkTxDrops  = 0;
static LinkDecoder      *linkDec      = nullptr;   // the receive task's; `a` reads its counters
static volatile uint32_t linkRxBytes  = 0;

// The node's view of its front end: a state heard within LINK_LOST_MS, and
// whether it carried a sample rate this mesh cannot play (TODO.md, 44.1 kHz).
static volatile bool          linkFe      = false;
static volatile unsigned long linkFeMs    = 0;
static volatile bool          linkRateBad = false;
static volatile uint32_t      linkBadState = 0;

// A command line one way, its answer the other: one slot each, filled by the
// receive task and emptied by loop().
static portMUX_TYPE    linkMsgMux     = portMUX_INITIALIZER_UNLOCKED;
static char            linkCmd[MESH_COMMAND_MAX + 1];   // front end: from the node
static volatile bool   linkCmdReady   = false;
static char           *linkReply      = nullptr;   // node: from the front end, LINK_MAX_PAYLOAD + 1
static volatile size_t linkReplyLen   = 0;
static volatile bool   linkReplyReady = false;

// The front end: its state has changed, send it now rather than at the next
// interval.
static volatile bool linkStateDirty = false;

/**
 * One frame onto the link. `mayDrop`: audio, which is sent only if the UART's
 * buffer has room for all of it now; anything else waits for room.
 */
static bool linkSend(uint8_t type, const uint8_t *payload, size_t len, bool mayDrop) {
    if (!linkUp) return false;
    if (xSemaphoreTake(linkTxLock, mayDrop ? 0 : pdMS_TO_TICKS(50)) != pdTRUE) {
        if (mayDrop) linkTxDrops++;
        return false;
    }
    bool         ok = false;
    const size_t n  = linkEncode(linkTxBuf, LINK_FRAME_MAX, type, linkTxSeq, payload, len);
    size_t       room = 0;
    if (n > 0 && (!mayDrop || (uart_get_tx_buffer_free_size(LINK_UART_NUM, &room) == ESP_OK &&
                               room >= n))) {
        uart_write_bytes(LINK_UART_NUM, (const char *)linkTxBuf, n);
        linkTxSeq++;
        linkTxFrames++;
        ok = true;
    } else if (mayDrop) {
        linkTxDrops++;
    }
    xSemaphoreGive(linkTxLock);
    return ok;
}

/** The front end's half of an A2DP packet: whole frames, in chunks a frame holds. */
static void linkSendAudio(const uint8_t *data, uint32_t length) {
    const uint32_t chunk = LINK_AUDIO_CHUNK * CLIENT_FRAME_BYTES;
    length &= ~(uint32_t)(CLIENT_FRAME_BYTES - 1);
    for (uint32_t off = 0; off < length; off += chunk) {
        const uint32_t n = length - off < chunk ? length - off : chunk;
        linkSend(LINK_AUDIO, data + off, n, true);
    }
}

// ============================================================================
// The server's source — a phone, by this chip's Bluetooth or a front end's
// ============================================================================
//
// What follows serves either: a phone on this chip's own A2DP sink, or on a
// front end's, whose packets and state arrive over the link (D18). Either way
// the packets go through a2dpDataCallback into this node's ring and the mesh.

volatile bool btConnected       = false;   // a phone is connected, whichever chip has it
volatile bool doConnectSound    = false;
volatile bool doDisconnectSound = false;


// Off with `f` over serial: the server keeps playing locally and stops handing
// frames to the radio, so one Bluetooth session can be measured with and
// without the mesh's transmissions competing for the radio.
static volatile bool a2dpForward = true;

// The server's own speaker plays from its own ring, as a client's does: the
// A2DP callback pushes every packet into it, and loop() plays it through
// driveRingI2S. Set once enterServer() has emptied the ring and installed the
// driver, cleared before either is torn down -- the callback is the ring's
// producer, and nothing may reset a ring its producer is writing.
//
// Until 2026-09-30 the A2DP library wrote I2S itself, through a 46 ms DMA
// ring, while clients played through a 91 ms prefill: measured with a
// microphone, the S3 44-51 ms behind the server, a plain echo between rooms.
// A server that plays through the same path as its clients can tell them
// exactly when it plays each block (`due`), and has a buffer as deep as
// theirs against Bluetooth's own pauses. See D14.
static volatile bool loopbackOn = false;

// How the Bluetooth task delivers audio, as seen from the stream reader: once
// per decoded packet, after the library's volume. `gap` is the time between
// two callbacks -- the Bluetooth stack's own jitter, which the ring has to
// ride out -- and `cb` how long our callback took (the forwarding). Printed
// and reset by `a`.
static volatile uint32_t a2dpPackets    = 0;
static volatile uint32_t a2dpBytes      = 0;
static volatile uint32_t a2dpPktMin     = UINT32_MAX;
static volatile uint32_t a2dpPktMax     = 0;
static volatile uint32_t a2dpGapMaxUs   = 0;
static volatile uint32_t a2dpCbMaxUs    = 0;
static volatile uint32_t a2dpGapHist[8];   // 5 ms buckets, last one open-ended
static uint32_t          a2dpLastUs     = 0;
static unsigned long     a2dpWindowMs   = 0;

static void a2dpStatsReset() {
    a2dpPackets = a2dpBytes = 0;
    a2dpPktMin = UINT32_MAX;
    a2dpPktMax = a2dpGapMaxUs = a2dpCbMaxUs = 0;
    for (auto &h : a2dpGapHist) h = 0;
    for (auto &h : txLatHist) h = 0;
    txLatMaxUs = 0;
    a2dpLastUs = 0;   // the first packet after a reset has no gap to measure
    a2dpWindowMs = millis();
}

static void a2dpStatsPrint() {
    const float secs = (millis() - a2dpWindowMs) / 1000.0f;
    DEBUG_SERIAL.printf(
        "[A2DP] win=%.1fs pk=%lu pk/s=%.1f pkB=%lu..%lu gapmax=%.1fms cbmax=%.2fms "
        "jit=%d und=%lu ovf=%lu dry=%lu fwd=%d wifi=%d heap=%lu gap5ms=",
        secs, (unsigned long)a2dpPackets, a2dpPackets / secs,
        (unsigned long)(a2dpPackets ? a2dpPktMin : 0), (unsigned long)a2dpPktMax,
        a2dpGapMaxUs / 1000.0f, a2dpCbMaxUs / 1000.0f,
        jbuf.fill(), (unsigned long)rxUnderrun, (unsigned long)rxOverflow,
        (unsigned long)outDry, a2dpForward ? 1 : 0, espnowActive ? 1 : 0,
        (unsigned long)ESP.getFreeHeap());
    for (int i = 0; i < 8; i++)
        DEBUG_SERIAL.printf(i ? ",%lu" : "%lu", (unsigned long)a2dpGapHist[i]);
    DEBUG_SERIAL.printf(" tx=%lu txlat=", (unsigned long)txSent);
    for (int i = 0; i < 8; i++)
        DEBUG_SERIAL.printf(i ? ",%lu" : "%lu", (unsigned long)txLatHist[i]);
    DEBUG_SERIAL.printf(" txlatmax=%.1fms rep=%u dist=%u par=%d pace=%lu mute=%d qfull=%lu\n",
                        txLatMaxUs / 1000.0f, txRepeat, txDistance, txParity ? 1 : 0,
                        (unsigned long)txPaceUs,
                        outputMuted ? 1 : 0, (unsigned long)txQueueFull);
}

static void a2dpForwardPacket(const uint8_t *data, uint32_t length, uint32_t first);

// Called from the BT task, once per decoded packet, after the library's
// volume. Into this node's own ring for its speaker, then to the mesh -- both
// in the same frame numbering, which is what lets a packet say when its block
// plays here.
static void a2dpDataCallback(const uint8_t *data, uint32_t length) {
    const uint32_t t0 = micros();
    a2dpPackets++;
    a2dpBytes += length;
    if (length < a2dpPktMin) a2dpPktMin = length;
    if (length > a2dpPktMax) a2dpPktMax = length;
    if (a2dpLastUs) {
        const uint32_t gap = t0 - a2dpLastUs;
        if (gap > a2dpGapMaxUs) a2dpGapMaxUs = gap;
        a2dpGapHist[min<uint32_t>(gap / 5000, 7)]++;
    }
    a2dpLastUs = t0;

    if (frontEnd) {   // the node plays and forwards it (D18)
        linkSendAudio(data, length);
        const uint32_t cb = micros() - t0;
        if (cb > a2dpCbMaxUs) a2dpCbMaxUs = cb;
        return;
    }

    const uint32_t first = ringPushed;
    if (loopbackOn) {
        const int bytes = (int)(length & ~(uint32_t)(CLIENT_FRAME_BYTES - 1));
        if (jbuf.pushBlock(data, bytes)) ringPushed += bytes / CLIENT_FRAME_BYTES;
        else                             rxOverflow++;
    }
    a2dpForwardPacket(data, length, first);

    const uint32_t cb = micros() - t0;
    if (cb > a2dpCbMaxUs) a2dpCbMaxUs = cb;
}

// Encodes the decoded A2DP stream -- 44.1 kHz stereo, after the volume -- into
// mesh packets, frame by frame. `first` is the ring index of the packet's first
// frame, whether or not this node's own ring took it.
static void a2dpForwardPacket(const uint8_t *data, uint32_t length, uint32_t first) {
    if (currentMode != MODE_SERVER || !txReady || !espnowActive || !a2dpForward) return;
    // Signed, for the reason spelled out at the ESP-NOW silence check: this
    // timestamp is written from the A2DP state callback, and an unsigned
    // difference against a timestamp set a moment in the future wraps to a huge
    // number — here that would silently skip the warmup instead of enforcing it.
    if ((long)(millis() - audioStartMs) < (long)TX_WARMUP_MS) return;

    const int16_t *in     = (const int16_t *)data;
    const int      frames = length / 4;   // 4 bytes per stereo frame
    for (int i = 0; i < frames; i++) meshTxFrame(in[2 * i], in[2 * i + 1], first + i);
}

#ifdef ENABLE_BLUETOOTH

BluetoothA2DPSink a2dpSink;
// Not `btStarted` — Arduino's esp32-hal-bt.h already declares `bool btStarted()`
// at global scope, and the collision is a hard compile error.
static bool   btSinkStarted     = false;

static void btConnectionChanged(esp_a2d_connection_state_t state, void *) {
    linkStateDirty = true;   // a front end tells its node at once
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
    linkStateDirty = true;
    if (state == ESP_A2D_AUDIO_STATE_STARTED) {
        txReady      = true;
        audioStartMs = millis();
        a2dpLastUs   = 0;   // the first packet of a stream has no gap
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

static void startBluetooth() {
    if (btSinkStarted) return;

    bringUpBtController();

    // `false`: the library's own I2S output stays off -- it installs no driver
    // and writes nothing. This node's speaker is played from the ring, like a
    // client's (loopbackOn); the callback still gets every packet, after the
    // library's volume.
    a2dpSink.set_stream_reader(a2dpDataCallback, false);
    a2dpSink.set_on_connection_state_changed(btConnectionChanged);
    a2dpSink.set_on_audio_state_changed(btAudioChanged);
    a2dpSink.set_auto_reconnect(false);
    a2dpSink.set_volume(VOLUME_DEFAULT);
    a2dpSink.start(roomName);
    btSinkStarted = true;
    LOG_INFO(String("BT discoverable as: ") + roomName);
    LOG_INFO("Heap after BT start: " + String(ESP.getFreeHeap()) + " bytes, maxalloc " +
             String(ESP.getMaxAllocHeap()));
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
    a2dpSink.end(false);
    const esp_err_t e1 = esp_bluedroid_disable();
    const esp_err_t e2 = esp_bt_controller_disable();
    btSinkStarted = false;
    LOG_INFO(String("BT stopped, controller off, memory kept (") + esp_err_to_name(e1) + "/" +
             esp_err_to_name(e2) + ")");
}

#endif  // ENABLE_BLUETOOTH

/**
 * Play a connect/disconnect jingle on a server's speaker.
 *
 * From loop(), into the output driver that loop() also feeds from the ring, so
 * the two cannot interleave: while the jingle plays the ring is simply not
 * read, and the stream carries on into it (and to the mesh, which never
 * notices). Afterwards the ring re-arms from the prefill, as at any start, and
 * the clients follow the new schedule.
 *
 * Before the server played from its own ring, the library wrote each packet
 * into the same driver from the Bluetooth task, and a jingle during a stream
 * came out chopped and stretched: "the 3 tones were very laggy" (2026-09-28).
 */
static void playJingle(void (*jingle)()) {
    schedInvalidate();
    jingle();
    jReady    = false;
    outSynced = false;
}

// ----------------------------------------------------------------------------
// The link's receiving end, both roles
// ----------------------------------------------------------------------------

/**
 * A front end's state, on the node: the same transitions the A2DP library's
 * callbacks make on a node with its own Bluetooth.
 */
static void linkOnState(const uint8_t *payload, size_t len) {
    LinkState s;
    if (!linkUnpackState(payload, len, &s)) {
        linkBadState++;   // another protocol version: a front end on other firmware
        return;
    }
    linkFeMs = millis();
    linkFe   = true;
    if (!linkSourceAllowed()) return;   // its own Bluetooth is this node's way in

    // 44.1 kHz is assumed from the codec to the schedule (TODO.md). A stream
    // at another rate would play at the wrong pitch in every room: refuse it.
    linkRateBad = s.streaming && s.sampleRate != CLIENT_SAMPLE_RATE;
    const bool streaming = s.streaming && !linkRateBad;

    if (s.connected != btConnected) {
        btConnected = s.connected;
        if (s.connected) doConnectSound = true;
        else             doDisconnectSound = true;
        LOG_INFO(s.connected ? "Front end: phone connected" : "Front end: phone disconnected");
    }
    if (streaming != txReady) {
        if (streaming) {
            txReady      = true;
            audioStartMs = millis();
            a2dpLastUs   = 0;
            meshTxReset();
        } else {
            txReady      = false;
            audioStartMs = 0;
        }
        LOG_INFO(streaming ? "Front end: audio started" : "Front end: audio stopped");
    }
}

static void linkOnFrame(const LinkFrame &f) {
    switch (f.type) {
        case LINK_AUDIO:
            if (!frontEnd && currentMode == MODE_SERVER && linkSourceAllowed()) {
                a2dpDataCallback(f.payload, f.len);
            }
            break;
        case LINK_STATE:
            if (!frontEnd) linkOnState(f.payload, f.len);
            break;
        case LINK_CMD:
            if (frontEnd && f.len > 0 && f.len <= MESH_COMMAND_MAX) {
                portENTER_CRITICAL(&linkMsgMux);
                memcpy(linkCmd, f.payload, f.len);
                linkCmd[f.len] = '\0';
                linkCmdReady   = true;
                portEXIT_CRITICAL(&linkMsgMux);
            }
            break;
        case LINK_REPLY:
            if (!frontEnd) {
                portENTER_CRITICAL(&linkMsgMux);
                memcpy(linkReply, f.payload, f.len);
                linkReply[f.len] = '\0';
                linkReplyLen     = f.len;
                linkReplyReady   = true;
                portEXIT_CRITICAL(&linkMsgMux);
            }
            break;
        default: break;   // a type from newer firmware: its CRC held, it is just not ours
    }
}

static void linkRxTask(void *) {
    uint8_t buf[512];
    while (true) {
        const int n = uart_read_bytes(LINK_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(5));
        if (n <= 0) continue;
        linkRxBytes += n;
        const uint8_t *p    = buf;
        size_t         left = (size_t)n;
        while (true) {
            LinkFrame    f;
            bool         got;
            const size_t used = linkDec->push(p, left, &f, &got);
            p    += used;
            left -= used;
            if (got) linkOnFrame(f);
            else if (left == 0) break;
        }
    }
}

/** The UART and its receive task, on a front end and on every node that may have one. */
static void linkBegin() {
    uart_config_t cfg = {};
    cfg.baud_rate  = LINK_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;
    esp_err_t err = uart_driver_install(LINK_UART_NUM, LINK_RX_BUFFER, LINK_TX_BUFFER, 0, nullptr, 0);
    if (err == ESP_OK) err = uart_param_config(LINK_UART_NUM, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(LINK_UART_NUM, LINK_TX_PIN, LINK_RX_PIN, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    linkTxLock = xSemaphoreCreateMutex();
    linkTxBuf  = (uint8_t *)malloc(LINK_FRAME_MAX);
    linkReply  = (char *)malloc(LINK_MAX_PAYLOAD + 1);
    linkDec    = new LinkDecoder();
    if (err != ESP_OK || linkTxLock == nullptr || linkTxBuf == nullptr || linkReply == nullptr ||
        linkDec == nullptr) {
        LOG_ERROR("Link UART failed: " + String(esp_err_to_name(err)));
        return;
    }
    // Nothing wired is a node without a front end, the usual case: its RX pin
    // reads idle rather than floating into noise.
    gpio_pullup_en((gpio_num_t)LINK_RX_PIN);
    xTaskCreatePinnedToCore(linkRxTask, "link_rx", LINK_TASK_STACK, nullptr, LINK_TASK_PRIO,
                            nullptr, 0);
    linkUp = true;
    LOG_INFO("Link UART up: RX " + String(LINK_RX_PIN) + ", TX " + String(LINK_TX_PIN) + " at " +
             String(LINK_BAUD) + " baud" + (frontEnd ? " (front end)" : ""));
}

// ============================================================================
// I2S output from the ring — every node that plays the stream
// ============================================================================
//
// A client plays what the mesh brings it; a Bluetooth server, since
// 2026-09-30, plays its own stream the same way (loopbackOn). One output path
// on every node, so every speaker runs on the same kind of schedule, and the
// server's can be told to the rest -- see lib/sync and D14.

static bool outI2SActive = false;

static void initI2SOutput() {
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
        LOG_ERROR("I2S install (output) failed: " + String(esp_err_to_name(err)));
        return;
    }
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);
    // A fresh driver fills its buffers from the start of the first: the frame
    // count the output clock relies on starts here.
    outFrames = 0;
    outClock.reset();
    outI2SActive = true;
    LOG_INFO("I2S output at " + String(CLIENT_SAMPLE_RATE) + " Hz " +
             (clientMonoOut ? "stereo, mixed to mono" : "stereo"));
}

static void deinitI2SOutput() {
    if (!outI2SActive) return;
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    outI2SActive = false;
    outClock.reset();
    schedInvalidate();
}

/**
 * Write to the output driver, counting frames and reading the output clock.
 *
 * A write that had to wait for a DMA buffer returns just after one finished
 * playing, and then the frames queued ahead of the next write are known
 * exactly (syncDmaAheadFrames). A write that did not wait says nothing, and
 * OUT_WAIT_MIN_US is what tells the two apart. If the next frame was already
 * due before this write, the DMA ran dry meanwhile and played zeros the frame
 * count knows nothing about: the clock is forgotten until the next wait.
 */
static size_t outWrite(const void *src, size_t bytes) {
    const uint32_t t0 = micros();
    if (outClock.valid() && (int32_t)(t0 - outClock.playUs(outFrames)) > 0) {
        outClock.reset();
        outDry++;
    }
    size_t bw = 0;
    i2s_write(I2S_NUM_0, src, bytes, &bw, pdMS_TO_TICKS(20));
    const uint32_t t1 = micros();
    outFrames += bw / CLIENT_FRAME_BYTES;
    if (bw > 0 && (t1 - t0) >= OUT_WAIT_MIN_US) {
        outClock.observe(t1, outFrames,
                         syncDmaAheadFrames(outFrames, CLIENT_DMA_BUF_COUNT, CLIENT_DMA_BUF_LEN));
    }
    return bw;
}

/** Play `frames` of silence: a client waiting for the server's schedule. */
static void outWriteSilence(uint32_t frames) {
    static const int16_t zeros[CLIENT_BATCH * 2] = {0};
    while (frames > 0) {
        const uint32_t n  = frames < (uint32_t)CLIENT_BATCH ? frames : (uint32_t)CLIENT_BATCH;
        const size_t   bw = outWrite(zeros, n * CLIENT_FRAME_BYTES);
        if (bw == 0) break;   // the DMA is not draining; do not spin
        frames -= bw / CLIENT_FRAME_BYTES;
    }
}

/** Let the oldest `frames` go unplayed. Whole frames, as everything here. */
static uint32_t ringDiscard(uint32_t frames) {
    const uint32_t have = (uint32_t)jbuf.fill() / CLIENT_FRAME_BYTES;
    if (frames > have) frames = have;
    jbuf.advance((int)(frames * CLIENT_FRAME_BYTES));
    ringConsumed += frames;
    return frames;
}

/** Close a timeline window every SYNC_WINDOW_MS; a flush forgets the lot. */
static void timelineService(unsigned long nowMs) {
    static unsigned long lastMs = 0;
    if ((long)(nowMs - lastMs) < (long)SYNC_WINDOW_MS) return;
    lastMs = nowMs;
    portENTER_CRITICAL(&timelineMux);
    if (timelineFlush) {
        timeline.clear();
        timelineFlush = false;
    } else {
        timeline.harvest(nowMs);
    }
    portEXIT_CRITICAL(&timelineMux);
}

/**
 * Start playing the ring, once it holds the prefill. Returns false to wait.
 *
 * A server starts at the prefill exactly, whatever piled up meanwhile (a
 * jingle leaves a full ring), and sets the schedule; everyone else follows it.
 *
 * A client of a server that stamps its packets waits for the server's
 * timeline -- a few hundred ms at the start of a stream, letting the oldest
 * audio go so the ring cannot overflow -- then primes the DMA with silence
 * until its own clock is known, and lets go of (or waits out) exactly what
 * separates its first frame from the server's schedule. Nothing has been
 * heard yet, so this costs nothing audible: it is where the 44-51 ms echo
 * measured before this existed is removed.
 *
 * A client that has heard no stamp for SYNC_MAX_AGE_MS, or waited SYNC_WAIT_MS
 * for a timeline that never came, plays as every client did before: from the
 * prefill, held at depth by the level controller.
 */
static bool outArm(unsigned long nowMs) {
    static unsigned long waitSinceMs = 0;
    const bool master = (currentMode == MODE_SERVER);

    if (jbuf.fill() < JITTER_PREFILL) {
        waitSinceMs = 0;
        return false;
    }

    const bool stamped  = !master && rxStampedMs &&
                          (long)(nowMs - rxStampedMs) < (long)SYNC_MAX_AGE_MS;
    const bool haveLine = stamped && timeline.valid(nowMs);
    if (stamped && !haveLine) {
        if (!waitSinceMs) waitSinceMs = nowMs;
        if ((long)(nowMs - waitSinceMs) < (long)SYNC_WAIT_MS) {
            ringDiscard((uint32_t)(jbuf.fill() - JITTER_PREFILL) / CLIENT_FRAME_BYTES);
            return false;
        }
    }
    waitSinceMs = 0;

    if (master) ringDiscard((uint32_t)(jbuf.fill() - JITTER_PREFILL) / CLIENT_FRAME_BYTES);

    outSynced = false;
    if (haveLine) {
        // Silence until the clock has been read a few times: the first waits
        // after an idle DMA can land on a ring still holding stale buffers.
        const uint32_t obs0 = outClock.observations;
        outClock.reset();
        for (int i = 0; i < 8 * CLIENT_DMA_BUF_COUNT &&
                        (!outClock.valid() || outClock.observations - obs0 < SYNC_PRIME_OBSERVATIONS); i++) {
            outWriteSilence(CLIENT_BATCH);
        }
        if (outClock.valid()) {
            const int32_t late = (int32_t)(outClock.playUs(outFrames) -
                                           timeline.dueUs(ringConsumed, nowMs));
            const int32_t n = syncUsToFrames(late, CLIENT_SAMPLE_RATE);
            uint32_t done = 0;
            if (n > 0)      done = ringDiscard((uint32_t)n);
            else if (n < 0) outWriteSilence((uint32_t)-n);
            DEBUG_SERIAL.printf("[SYNC] armed late=%ldus %s=%ld frames%s\n", (long)late,
                                n >= 0 ? "dropped" : "waited", (long)(n >= 0 ? done : -n),
                                (n > 0 && done < (uint32_t)n) ? " (ring short: playing late)" : "");
            outSynced = true;
            syncCtl.reset(nowMs);
        }
    }

    jReady = true;
    levelCtl().reset(nowMs);
    LOG_INFO("Ring ready (" + String(jbuf.fill()) + " bytes) — starting I2S" +
             (master ? ", setting the schedule" : outSynced ? ", on the server's schedule" : ""));
    return true;
}

// Feed I2S from the ring — every loop() pass on a node that is playing.
static void driveRingI2S() {
    if (!outI2SActive) return;

    const unsigned long nowMs = millis();
    timelineService(nowMs);

    if (!jReady && !outArm(nowMs)) return;

    // Interleaved L, R -- exactly the ring's layout and exactly what I2S takes.
    static int16_t stereo[CLIENT_BATCH * 2];
    static const int BATCH_BYTES = CLIENT_BATCH * CLIENT_FRAME_BYTES;

    if (!jbuf.peek((uint8_t *)stereo, BATCH_BYTES)) {
        // The ring is empty, but the DMA behind it still holds ~44 ms: until
        // that has played out, nothing is missing yet. Wait for the next packet
        // and carry on where it left off -- the schedule is untouched. Windows
        // pauses its A2DP stream for up to 54 ms, longer than the ring alone
        // holds, and treating an empty ring as an underrun turned each of those
        // pauses into a re-arm and a hole of a prefill's length: 8-10 a minute
        // on each client, 2026-09-30.
        if (outClock.valid() && (int32_t)(micros() - outClock.playUs(outFrames)) < 0) return;

        // Underrun: the DMA has run dry. tx_desc_auto_clear already zeroes the
        // DMA as it drains, so pushing extra silence here would only add a click.
        // Re-arm the prefill gate and let the buffer refill. Counted, not logged
        // — logging every loop tick during an underrun makes the underrun worse.
        jReady    = false;
        outSynced = false;
        rxUnderrun++;
        schedInvalidate();
        // The refill that follows is not drift. Integrating it would provoke a
        // burst of corrections on top of a buffer that is already unhappy.
        levelCtl().reset(nowMs);
        return;
    }

    // Where this node plays, published for whoever stamps packets with it
    // (the transmit task, on a server), and how far that is from the server's
    // schedule (on a client). The next frame of the ring is the next written.
    int32_t syncErr = 0;
    bool    haveErr = false;
    if (outClock.valid()) {
        const uint32_t nextUs = outClock.playUs(outFrames);
        schedPublish(ringConsumed, nextUs);
        if (outSynced && timeline.valid(nowMs)) {
            syncErr   = (int32_t)(nextUs - timeline.dueUs(ringConsumed, nowMs));
            syncErrUs = syncErr;
            haveErr   = true;
        }
    } else {
        schedInvalidate();
    }
    if (outSynced && !timeline.valid(nowMs) && !(rxStampedMs &&
            (long)(nowMs - rxStampedMs) < (long)SYNC_MAX_AGE_MS)) {
        // The stamps stopped: carry on at depth, as a client of an older server.
        outSynced = false;
        levelCtl().reset(nowMs);
    }

    // Too far off to steer: something moved the schedule by more than a
    // correction per batch could follow in reasonable time -- a server that
    // re-armed, this node's DMA running dry. Jump: skip what is late, or wait
    // out what is early. Audible, but so was whatever caused it.
    if (haveErr && (syncErr > SYNC_JUMP_US || syncErr < -SYNC_JUMP_US)) {
        const int32_t n = syncUsToFrames(syncErr, CLIENT_SAMPLE_RATE);
        if (n > 0) ringDiscard((uint32_t)n);
        else       outWriteSilence((uint32_t)-n);
        syncJumps++;
        syncCtl.reset(nowMs);
        return;
    }

    // Ask before the batch, apply after it. Keeping the correction outside the
    // batch is what leaves the partial-write accounting below untouched: one
    // frame either side of a write whose length is already handled correctly,
    // rather than an edit in the middle of a buffer that I2S may only half take.
    //
    // On the server's schedule the error is time, not depth: the same
    // controller, handed that error as if it were a fill above a fixed target.
    DriftController::Correction corr = DriftController::NONE;
    if (driftEnabled) {
        if (outSynced) {
            if (haveErr) corr = syncCtl.update(nowMs, SYNC_FILL_BASE + syncUsToBytes(syncErr));
        } else {
            corr = levelCtl().update(nowMs, jbuf.fill());
        }
    }
    DriftController &ctl = outSynced ? syncCtl : levelCtl();

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
    if (!outputMuted) applyGainQ12(stereo, CLIENT_BATCH * 2, outTrimQ12);

    // Blocking with a short timeout paces this loop to the I2S sample clock.
    const size_t bw = outWrite(stereo, BATCH_BYTES);

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
    ringConsumed += frames;

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
        if (ringDiscard(1) == 1) ctl.confirm(corr);
    } else if (corr == DriftController::INSERT && frames > 0) {
        // Play a frame twice without consuming it: hold it one extra sample
        // period. No discontinuity is possible by construction, because it is
        // the frame that was just played.
        int16_t frame[2] = {stereo[2 * (frames - 1)], stereo[2 * (frames - 1) + 1]};
        if (outWrite(frame, sizeof(frame)) == sizeof(frame)) ctl.confirm(corr);
    }
}

// ============================================================================
// Mode Transitions
// ============================================================================

// Set when entering CLIENT mode failed, so the next attempt is held off.
static unsigned long clientRetryAfterMs = 0;

/** The ring and everything that describes it, for a new stream. Producer stopped. */
static void resetPlayState() {
    ringReset();
    rxHoles.clear();
    rxStore.clear();
    rxPrevValid = false;
    seqTracker.reset();
    portENTER_CRITICAL(&traceMux);
    rxTrace.reset();
    portEXIT_CRITICAL(&traceMux);
    driftCtl.reset(millis());
    srvCtl.reset(millis());
    syncCtl.reset(millis());
    portENTER_CRITICAL(&timelineMux);
    timeline.clear();
    timelineFlush = false;
    portEXIT_CRITICAL(&timelineMux);
    rxStampedMs = 0;
    jReady      = false;
    outSynced   = false;
    schedInvalidate();
}

static void resetRxState() {
    resetPlayState();
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

    // A server's own ring stops taking the stream before anything resets it;
    // the Bluetooth task, or the link's (D18), may be inside a push, and a
    // push takes microseconds.
    if (loopbackOn) {
        loopbackOn = false;
        delay(5);
    }
    deinitI2SOutput();   // a client's, or a Bluetooth server's own

    // Reset on *every* entry to DISCOVERY, not just when leaving CLIENT.
    // senderLocked used to survive SERVER → DISCOVERY: a node that had once
    // heard server B would then silently ignore every broadcast from server C
    // and could never join the mesh again without a power cycle. rxActive
    // leaking the same way sent the node straight into CLIENT mode on a stream
    // that had stopped minutes earlier.
    resetRxState();

    // Leaving CLIENT does not start Bluetooth again (D17). It used to, so a
    // phone could find the node, and every stream after a pause of more than
    // ESPNOW_SILENCE_TIMEOUT_MS then found it with Bluetooth to stop first: a
    // WROVER came in 0.77-0.88 s after the other rooms, at every start
    // (2026-10-06). A short press opens Bluetooth for a phone (btOpenWindow).
    // Leaving SERVER never stopped it, so a phone that disconnects can still
    // come back.

    currentMode = MODE_DISCOVERY;
    LOG_INFO("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
}

static void enterServer() {
    LOG_INFO("=== SERVER — local playback + ESP-NOW broadcast ===");
    // BT is already running (started in setup). The data callback will start
    // forwarding once BT audio state goes STARTED.
    meshTxReset();
    // Its own speaker plays from its own ring, like a client's, so that the
    // packets it sends can say when it plays them (loopbackOn, D14). Ring
    // first, then the driver, then the producer -- the A2DP callback, here or
    // on a front end (D18).
    if (ownBt() || linkSourceAllowed()) {
        resetPlayState();
        rxOverflow = rxUnderrun = 0;
        initI2SOutput();
        loopbackOn = outI2SActive;
        if (!outI2SActive) LOG_ERROR("SERVER I2S unavailable — mesh only, no local playback");
    }
    currentMode = MODE_SERVER;
}

static void enterClient() {
    LOG_INFO("=== CLIENT — ESP-NOW → I2S ===");

#ifdef ENABLE_BLUETOOTH
    // Only when it is running: since D17 a node that has been a client before
    // has it off already, and the 200 ms would make this room late for nothing.
    const bool hadBt = btSinkStarted;
    if (btAllowed() && hadBt) {
        LOG_INFO("Stopping BT to release I2S...");
        stopBluetooth();
        delay(200);
    }
#endif

    resetPlayState();
    rxActive = false;

    initI2SOutput();
    if (!outI2SActive) {
        LOG_ERROR("CLIENT I2S unavailable — returning to DISCOVERY");
#ifdef ENABLE_BLUETOOTH
        if (btAllowed() && hadBt) startBluetooth();
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

enum MeshFeedback { MESH_FB_OPEN, MESH_FB_PAIRED, MESH_FB_CLOSED, MESH_FB_TICK };

/**
 * Say what just happened, on the speaker the node already has.
 *
 * This is the only acknowledgement a user without a serial console ever gets,
 * and pairing without it is a button that appears to do nothing.
 *
 * Only in DISCOVERY. In CLIENT and SERVER modes the output driver is playing
 * the ring on a schedule other nodes follow, and a tone in the middle of it
 * would move that schedule. Both are also cases where the user can hear
 * perfectly well that the node is alive.
 */
static void meshPlayFeedback(MeshFeedback what) {
    if (currentMode != MODE_DISCOVERY) return;

    // Nobody owns the driver in DISCOVERY: the A2DP library never installs
    // one (its output is off, see startBluetooth), and the ring's output is
    // installed only in CLIENT and SERVER. The check stays, because installing
    // a second driver over a first fails and the tone would be lost.
    const bool someoneElseOwnsI2S = outI2SActive;
    if (!someoneElseOwnsI2S) initI2SForTones();

    switch (what) {
        case MESH_FB_OPEN:   playStartupSound();      break;   // two beeps: I am listening
        case MESH_FB_PAIRED: playConnectedSound();    break;   // rising: joined
        case MESH_FB_CLOSED: playDisconnectedSound(); break;   // falling: nothing heard
        case MESH_FB_TICK:   playTickSound();         break;   // one: let go now
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

    TxItem item;
    item.first     = 0;
    item.pkt.group = wireGroup(meshId);
    item.pkt.seq   = MESH_BEACON_SEQ;
    item.pkt.len   = 0;            // the mesh id in the header is the whole message
    item.pkt.due   = MESH_DUE_NONE;
    if (xQueueSend(txQueue, &item, 0) != pdTRUE) txQueueFull++;
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

// ============================================================================
// Bluetooth on demand — the short press (D17)
// ============================================================================
//
// A node that can be a server starts Bluetooth at boot and keeps it on until
// it first plays as a client. From then on it stays off: the node joins every
// stream at once, and its radio is the mesh's alone. A short press, or `B`,
// turns it on for BT_WINDOW_MS so a phone can connect.

/** Bluetooth on for BT_WINDOW_MS. The short press, and `B`. */
static void btOpenWindow() {
#ifdef ENABLE_BLUETOOTH
    if (frontEnd) {
        DEBUG_SERIAL.println("[BT] on for good (a front end, D18)");
        return;
    }
    if (cfgPausedBt) {
        DEBUG_SERIAL.println("[BT] error reason=config (paused while the setup network is open)");
        return;
    }
    if (!btAllowed()) {
        DEBUG_SERIAL.println("[BT] error reason=no_bt (client-only, or bench mode)");
        return;
    }
    if (currentMode == MODE_SERVER) {
        DEBUG_SERIAL.println("[BT] error reason=serving (a phone is connected already)");
        return;
    }
    // Stop playing first, as pairing does. The music stopping and then the two
    // drops are what tell somebody with no console that the press registered,
    // and a node that went on playing could not take the phone's stream: it is
    // kept out of CLIENT until the window closes.
    if (currentMode == MODE_CLIENT) enterDiscovery();
    startBluetooth();   // does nothing if it is already running
    btWindow        = true;
    btWindowUntilMs = millis() + BT_WINDOW_MS;
    DEBUG_SERIAL.printf("[BT] window open %lus: discoverable as %s\n",
                        (unsigned long)(BT_WINDOW_MS / 1000), roomName);
    // Not here: `B` over the mesh answers with what this printed, and 1.2 s
    // of drops inside the command would make the answer late for its asker.
    btWindowTone = true;
#else
    DEBUG_SERIAL.println("[BT] error reason=no_bt (this chip has no Bluetooth Classic)");
#endif
}

/**
 * Close the window: a phone connected, or none did in time. From loop(), in
 * every mode, before the mode switch acts on btConnected.
 */
static void btServiceWindow() {
#ifdef ENABLE_BLUETOOTH
    if (!btWindow) return;
    if (btWindowTone) {
        btWindowTone = false;
        meshPlayFeedback(MESH_FB_OPEN);   // two drops, as pairing's window opening
    }
    if (btConnected) {
        btWindow = false;
        DEBUG_SERIAL.println("[BT] window closed: connected");
        return;
    }
    if ((long)(millis() - btWindowUntilMs) < 0) return;
    btWindow = false;
    stopBluetooth();
    DEBUG_SERIAL.println("[BT] window closed: nobody connected, Bluetooth off");
    // The three falling drops, unless the mesh is playing: then the music
    // coming back says the same, and the 1.6 s tone would only make this room
    // late joining it.
    if (!rxActive) meshPlayFeedback(MESH_FB_CLOSED);
#endif
}

static void configOpen(uint32_t windowMs);

/**
 * The button, read by how long it was held when it is let go (lib/button):
 * under a second opens Bluetooth (btOpenWindow), 3 to 9 seconds pairs, 10 to
 * 15 opens the setup network (D19). Between the ranges, and past the last, a
 * release does nothing.
 *
 * A press *while running*, never a press held through a reset: the BOOT button
 * these boards use is a strapping pin, and holding it across a reset puts the
 * chip into the ROM download mode, where no firmware runs at all.
 *
 * Every range acts on release, so a hold on its way to a longer one does
 * nothing on the way; a drop says when a hold has reached the next range, in
 * DISCOVERY, where a tone can play. Pairing used to act as the hold reached
 * 3 s; it waits for the release now, or a hold on its way to the setup
 * network would open a pairing window as it went past.
 */
static void meshServiceButton() {
#if MESH_PAIR_BUTTON_PIN >= 0
    static const ButtonTimes times = {BUTTON_PRESS_MIN_MS, BUTTON_BT_MAX_MS,
                                      BUTTON_PAIR_MIN_MS,  BUTTON_PAIR_MAX_MS,
                                      BUTTON_CONFIG_MIN_MS, BUTTON_CONFIG_MAX_MS};
    static bool          held        = false;
    static unsigned long heldSinceMs = 0;
    static uint32_t      lastHeldMs  = 0;

    const bool down = (digitalRead(MESH_PAIR_BUTTON_PIN) == LOW);

    if (!held) {
        if (down) {
            held        = true;
            heldSinceMs = millis();
            lastHeldMs  = 0;
        }
        return;
    }

    const uint32_t heldMs = (uint32_t)(millis() - heldSinceMs);
    if (down) {
        // "Let go now": a drop as the hold enters the next range. It holds
        // loop() for a second or so, which the hold's length then includes.
        if (buttonEntered(lastHeldMs, heldMs, times) != BUTTON_NONE) meshPlayFeedback(MESH_FB_TICK);
        lastHeldMs = heldMs;
        return;
    }

    held = false;
    switch (buttonAction(heldMs, times)) {
        case BUTTON_BLUETOOTH:
            btOpenWindow();
            break;
        case BUTTON_PAIR:
            // Role decides which half of the handshake a press is. A node that
            // can be a server holds the mesh others join; a speaker is what
            // gets moved. Nothing for the user to pick, and it matches where
            // the two boxes physically are. A node with a front end (D18) is
            // one that can be a server.
            if (ownBt() || linkFe) meshStartOffering();
            else                   meshStartPairing();
            break;
        case BUTTON_CONFIG:
            configOpen(CONFIG_WINDOW_MS);
            break;
        case BUTTON_NONE:
            DEBUG_SERIAL.printf("[BUTTON] released after %lums: nothing\n", (unsigned long)heldMs);
            break;
    }
#endif
}

// ============================================================================
// Update mode — new firmware over the home WiFi (OTA)
// ============================================================================
//
// A node plugged into a stereo across the house has power and nothing else, so
// a new image has to arrive by radio. It comes over the home network, not the
// mesh: the router is on channel 1 and the mesh on ESPNOW_CHANNEL, and a
// station follows its access point's channel, so a node cannot be on both.
// Update mode is therefore a boot of its own, like bench mode -- flagged in RTC
// memory, entered through a restart, the mesh never started. See D15.
//
// The sequence, driven end to end by tools/ota.ps1:
//
//   1. `U<name>` on any node that is on USB broadcasts an update request for
//      the node with that ROOM_NAME or MAC, in this mesh only.
//   2. The named node reboots into update mode: joins the network stored by
//      `W`, and answers GET / with its room name -- in silence, see otaLeave().
//   3. The PC posts the image to /update. Once it is written and verified
//      the node boots it.
//   4. The new image is on probation until it has run OTA_CONFIRM_MS with its
//      radio up. Until then any reset boots the previous image instead.
//
// Nothing here runs unless asked: the endpoint exists for OTA_WINDOW_MS after
// a request and not otherwise, and any reset in the middle lands in normal mode.

#define OTA_MAGIC 0x0DA7E501
RTC_NOINIT_ATTR static uint32_t otaMagic;   // RTC_NOINIT for bench mode's reason

// What the node was receiving when the request reached it, carried across the
// restart for GET / to report. Before D16 a client transmitted nothing at all,
// so this was the only way to measure a speaker across the house: stream at
// it, ask, read. `@<name> r` now asks without a reboot; this stays because it
// also reads the home WiFi's signal where the node stands. The counters are
// the current session's, the ones the status line prints; they restart
// whenever the node enters CLIENT, so ask while the stream is still playing.
#define OTA_SNAP_MAGIC 0x0DA75AA5
struct OtaSnapshot {
    uint32_t magic, upMs, rx, lost, ovf, und, rec, late, dup, rsy, fgn, sjmp, dry;
    uint32_t runs[8];
    uint8_t  mode;
};
RTC_NOINIT_ATTR static OtaSnapshot otaSnap;

/**
 * Keep the rollback decision for ourselves.
 *
 * The Arduino core would otherwise mark a new image good in initArduino(),
 * before setup() -- which leaves the bootloader's rollback catching only an
 * image that dies before main. With this the image stays on probation until
 * otaConfirm(), and a crash loop, a hang the watchdog ends, or a power cut in
 * the meantime boots the previous one. The bootloader in this core is built
 * with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, which is what makes that happen.
 */
extern "C" bool verifyRollbackLater() { return true; }

static bool otaPendingVerify() {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

/** Keep the running image. A no-op on anything that was flashed over USB. */
static void otaConfirm(const char *why) {
    if (!otaPendingVerify()) return;
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    DEBUG_SERIAL.printf("[OTA] image kept (%s) -> %s\n", why, esp_err_to_name(err));
}

/**
 * The probation. Called from loop() in every mode.
 *
 * "Ran for a minute" alone would keep an image whose radio never came up, and
 * that is the one image that must not be kept: it can never hear another
 * update request, so the only way back would be a USB cable.
 */
static void otaServiceConfirm() {
    static bool done = false;
    if (done || millis() < OTA_CONFIRM_MS) return;
    if (espnowWanted && !espnowActive) return;
    done = true;
    otaConfirm("ran");
}

/** Which slot is running, whether it is on probation, and whether one was rolled back. */
static void otaDescribe(char *out, size_t outSize) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *failed  = esp_ota_get_last_invalid_partition();
    snprintf(out, outSize, "fw=%s chip=%s mac=%s name=%s mesh=%04X app=%s pending=%d rolledback=%s",
             FW_VERSION, ESP.getChipModel(), ownMac, roomName, meshId,
             running ? running->label : "?", otaPendingVerify() ? 1 : 0,
             failed ? failed->label : "-");
}

/** ROOM_NAME as a DHCP hostname: SonoLoco-WROVER2 is sonoloco-wrover2 on the router. */
static void otaHostname(char *out, size_t outSize) {
    size_t n = 0;
    for (const char *p = roomName; *p && n + 1 < outSize; p++) {
        const char c = (char)tolower((unsigned char)*p);
        out[n++] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '-';
    }
    out[n] = '\0';
}

/** Leave for update mode. The request was heard, so the image works: keep it first. */
static void otaRebootIntoUpdate() {
    // Otherwise the restart below would count as the failed boot that
    // rolls a perfectly good image back.
    otaConfirm("update requested");

    otaSnap.upMs = millis();
    otaSnap.rx   = rxCount;
    otaSnap.lost = seqTracker.lost;
    otaSnap.ovf  = rxOverflow;
    otaSnap.und  = rxUnderrun;
    otaSnap.rec  = rxRecovered;
    otaSnap.late = rxLate;
    otaSnap.dup  = seqTracker.dupe;
    otaSnap.rsy  = seqTracker.resync;
    otaSnap.fgn  = rxForeign;
    otaSnap.sjmp = syncJumps;
    otaSnap.dry  = outDry;
    for (int i = 0; i < 8; i++) otaSnap.runs[i] = rxLossRuns[i];
    otaSnap.mode  = (uint8_t)currentMode;
    otaSnap.magic = OTA_SNAP_MAGIC;
    otaMagic = OTA_MAGIC;
    DEBUG_SERIAL.println("[OTA] rebooting into update mode");
    DEBUG_SERIAL.flush();
    delay(50);
    ESP.restart();
}

/**
 * `U<name>`: ask the node with that ROOM_NAME or MAC to reboot into update mode.
 *
 * `@<name> U` does the same as a command (D16), but this keeps the update
 * request's own wire format, the one every node since D15 understands: an
 * update is how a node gets the firmware that understands commands, so the
 * request for it cannot be one. Sent by meshServiceControl().
 */
static void otaStartRequest(const char *target) {
    const size_t n = strlen(target);
    if (!espnowActive || txQueue == nullptr) {
        DEBUG_SERIAL.println("[OTA] error reason=no_mesh");
        return;
    }
    if (n == 0 || n > MESH_TARGET_MAX) {
        DEBUG_SERIAL.println("[OTA] error reason=target_length");
        return;
    }
    ctrlSend(MESH_UPDATE_SEQ, (const uint8_t *)target, n, OTA_REQUEST_COPIES,
             OTA_REQUEST_INTERVAL_MS);
    DEBUG_SERIAL.printf("[OTA] request target=%s mesh=%04X copies=%d\n",
                        target, meshId, OTA_REQUEST_COPIES);
}

enum OtaOutcome { OTA_UPDATED, OTA_FAILED, OTA_DISMISSED };

/**
 * Leave update mode and boot whatever is due.
 *
 * Without a sound, whichever way it went. The node is plugged into somebody's
 * stereo, and the PC that asked already knows the outcome -- it reads it over
 * HTTP. The first version played the pairing tones here and on the way in,
 * which with the startup sound came to six tones into a living room for one
 * update. Pairing keeps them because there a person is standing at the node.
 */
[[noreturn]] static void otaLeave(OtaOutcome how) {
    DEBUG_SERIAL.printf("[OTA] leaving update mode updated=%d\n", how == OTA_UPDATED ? 1 : 0);
    DEBUG_SERIAL.flush();
    ESP.restart();
    while (true) {}
}

// What the HTTP handlers, on the server's own task, tell otaRun()'s loop.
static volatile bool otaReceiving = false;   // an upload is in progress
static volatile int  otaResult    = 0;       // 1 written, -1 failed, 0 nothing yet
static volatile bool otaExitAsked = false;

// Set by the WiFi and IP event handler while update mode joins the network.
static volatile bool     otaGotIp = false;
static esp_ip4_addr_t    otaIp    = {};

static void otaOnNetEvent(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();   // and again after every failure, until the timeout
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        otaIp    = ((ip_event_got_ip_t *)data)->ip_info.ip;
        otaGotIp = true;
    }
}

/** GET / -- who is this and what is it running. The PC reads it before uploading. */
static esp_err_t otaHttpIdentify(httpd_req_t *req) {
    char info[640];
    otaDescribe(info, sizeof(info));
    size_t n = strlen(info);

    // The home WiFi as this node hears it, where it stands.
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        n += snprintf(info + n, sizeof(info) - n, " rssi=%d ch=%d", (int)ap.rssi, (int)ap.primary);
    }
    // And the mesh as it heard it, up to the request (see OtaSnapshot).
    if (otaSnap.magic == OTA_SNAP_MAGIC && n < sizeof(info)) {
        const uint8_t m = otaSnap.mode;
        n += snprintf(info + n, sizeof(info) - n,
            " before=%s up=%lus rx=%lu lost=%lu ovf=%lu und=%lu rec=%lu late=%lu dup=%lu"
            " rsy=%lu fgn=%lu sjmp=%lu dry=%lu runs=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
            m == MODE_CLIENT ? "CLIENT" : m == MODE_SERVER ? "SERVER" : "DISCOVERY",
            (unsigned long)(otaSnap.upMs / 1000), (unsigned long)otaSnap.rx,
            (unsigned long)otaSnap.lost, (unsigned long)otaSnap.ovf, (unsigned long)otaSnap.und,
            (unsigned long)otaSnap.rec, (unsigned long)otaSnap.late, (unsigned long)otaSnap.dup,
            (unsigned long)otaSnap.rsy, (unsigned long)otaSnap.fgn, (unsigned long)otaSnap.sjmp,
            (unsigned long)otaSnap.dry,
            (unsigned long)otaSnap.runs[0], (unsigned long)otaSnap.runs[1], (unsigned long)otaSnap.runs[2],
            (unsigned long)otaSnap.runs[3], (unsigned long)otaSnap.runs[4], (unsigned long)otaSnap.runs[5],
            (unsigned long)otaSnap.runs[6], (unsigned long)otaSnap.runs[7]);
    }
    strlcat(info, "\n", sizeof(info));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, info);
}

/**
 * POST /update -- the image itself, as the raw request body.
 *
 * Written into the slot not running, erased as it goes. esp_ota_end() then
 * checks the whole image -- its checksum, its hash, and the chip it was built
 * for -- before anything is allowed to boot it, so a C3 image sent to an S3 is
 * refused here rather than discovered at the next boot.
 */
static esp_err_t otaHttpUpdate(httpd_req_t *req) {
    otaReceiving = true;
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t ota = 0;
    const char *failure = nullptr;
    esp_err_t err = ESP_OK;

    // Heap, and only for the length of the upload: static, it would cost a
    // Bluetooth server this much DRAM on every boot that never updates.
    const size_t chunk = 4096;
    char *buf = (char *)malloc(chunk);

    if (buf == nullptr) {
        failure = "no memory";
    } else if (slot == nullptr) {
        failure = "no update slot (partition table without OTA -- reflash over USB)";
    } else if (req->content_len == 0 || req->content_len > slot->size) {
        failure = "bad length";
    } else if ((err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota)) != ESP_OK) {
        failure = "begin";
    } else {
        DEBUG_SERIAL.printf("[OTA] receiving bytes=%u into %s\n",
                            (unsigned)req->content_len, slot->label);
        size_t left = req->content_len;
        int    timeouts = 0;
        while (left > 0 && failure == nullptr) {
            const int n = httpd_req_recv(req, buf, left < chunk ? left : chunk);
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
            if (n <= 0) { failure = "receive"; break; }
            timeouts = 0;
            if ((err = esp_ota_write(ota, buf, n)) != ESP_OK) failure = "write";
            left -= n;
        }
        if (failure != nullptr) {
            esp_ota_abort(ota);
        } else if ((err = esp_ota_end(ota)) != ESP_OK) {
            failure = "image rejected";
        } else if ((err = esp_ota_set_boot_partition(slot)) != ESP_OK) {
            failure = "set boot";
        }
    }
    free(buf);

    char reply[96];
    if (failure == nullptr) {
        snprintf(reply, sizeof(reply), "OK %s\n", slot->label);
        DEBUG_SERIAL.printf("[OTA] written slot=%s\n", slot->label);
    } else {
        snprintf(reply, sizeof(reply), "FAIL %s %s\n", failure, esp_err_to_name(err));
        DEBUG_SERIAL.printf("[OTA] error reason=%s err=%s\n", failure, esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, reply);

    otaResult    = failure == nullptr ? 1 : -1;
    otaReceiving = false;
    return ESP_OK;
}

/** POST /exit -- back to the mesh without an update, once the PC has read the version. */
static esp_err_t otaHttpExit(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "bye\n");
    otaExitAsked = true;
    return ESP_OK;
}

/**
 * The update-mode boot. Called from setup() instead of everything after the
 * banner, and never returns: it ends in a restart whatever happens.
 *
 * ESP-IDF directly, not the Arduino WiFi, WebServer, Update and mDNS classes.
 * Those worked, and cost 4 KB of static DRAM on every boot -- mDNS's packet
 * buffer, a DNS table, smartconfig's timers -- which is a fifth of what a
 * Bluetooth server has left while it streams (CHANGELOG). This way the cost is
 * heap, and only on the boot that updates. Without mDNS the PC finds the node
 * by asking the LAN who answers GET / with this ROOM_NAME (tools/ota.ps1).
 */
[[noreturn]] static void otaRun() {
    char info[256];
    otaDescribe(info, sizeof(info));
    DEBUG_SERIAL.printf("[OTA] update mode %s\n", info);

    wifi_config_t wc = {};
    prefs.begin(PREF_NAMESPACE, true);
    prefs.getString(PREF_WIFI_SSID, (char *)wc.sta.ssid, sizeof(wc.sta.ssid));
    prefs.getString(PREF_WIFI_PASS, (char *)wc.sta.password, sizeof(wc.sta.password));
    prefs.end();
    if (wc.sta.ssid[0] == '\0') {
        DEBUG_SERIAL.println("[OTA] error reason=no_wifi (set it with W, or tools/ota-wifi.ps1)");
        otaLeave(OTA_FAILED);
    }

    char host[33];
    otaHostname(host, sizeof(host));
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, host);   // the name the router's client list shows
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, otaOnNetEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, otaOnNetEvent, nullptr);

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wcfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // the credentials live in our namespace
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);            // modem sleep halves upload throughput

    const unsigned long t0 = millis();
    while (!otaGotIp) {
        if ((long)(millis() - t0) > (long)OTA_CONNECT_TIMEOUT_MS) {
            DEBUG_SERIAL.println("[OTA] error reason=wifi (wrong password, or out of range)");
            otaLeave(OTA_FAILED);
        }
        delay(100);
    }
    wifi_ap_record_t ap = {};
    esp_wifi_sta_get_ap_info(&ap);
    DEBUG_SERIAL.printf("[OTA] ready ip=" IPSTR " host=%s ch=%d rssi=%d\n",
                        IP2STR(&otaIp), host, (int)ap.primary, (int)ap.rssi);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;   // esp_ota_end() verifies the image on this stack
    httpd_handle_t http = nullptr;
    if (httpd_start(&http, &hc) != ESP_OK) {
        DEBUG_SERIAL.println("[OTA] error reason=http");
        otaLeave(OTA_FAILED);
    }
    const httpd_uri_t routes[] = {
        {"/",       HTTP_GET,  otaHttpIdentify, nullptr},
        {"/update", HTTP_POST, otaHttpUpdate,   nullptr},
        {"/exit",   HTTP_POST, otaHttpExit,     nullptr},
    };
    for (const httpd_uri_t &r : routes) httpd_register_uri_handler(http, &r);

    unsigned long windowStart = millis();
    while (true) {
        if (otaResult != 0 || otaExitAsked) {
            delay(300);   // let the reply leave before the radio does
            if (otaResult == 1) otaLeave(OTA_UPDATED);
            if (otaExitAsked)   otaLeave(OTA_DISMISSED);
            // A failed upload keeps the window open for another attempt.
            otaResult   = 0;
            windowStart = millis();
        }
        if (otaReceiving) windowStart = millis();
        if ((long)(millis() - windowStart) > (long)OTA_WINDOW_MS) {
            DEBUG_SERIAL.println("[OTA] window closed, nothing uploaded");
            otaLeave(OTA_FAILED);
        }
        delay(20);
    }
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

        // A source with no speaker still has a schedule: every frame plays
        // BENCH_PLAY_DELAY_US after it was due to be generated, which is what
        // its packets' `due` say -- so a bench run exercises the clients' sync
        // exactly as a Bluetooth server would.
        schedPublish(benchSampleIdx,
                     (uint32_t)(benchStartUs + BENCH_PLAY_DELAY_US +
                                (int64_t)benchSampleIdx * 1000000LL / CLIENT_SAMPLE_RATE));

        // One packet's worth of frames, through the same encoder a server uses.
        // Table lookup, wrapping by comparison rather than modulo: no division
        // and no float anywhere in the transmit path.
        const uint32_t fullBefore = txQueueFull;
        for (int i = 0; i < MESH_BLOCK_FRAMES; i++) {
            const int16_t s = benchTone[benchTonePhase];
            if (++benchTonePhase >= BENCH_TONE_LEN) benchTonePhase = 0;
            meshTxFrame(s, s, benchSampleIdx + i);
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
    schedInvalidate();
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
        "drift=%d ins=%lu drp=%lu dr=%.3f tgt=%d srx=%ld maxalloc=%lu rec=%lu "
        "sync=%d se=%ld sjmp=%lu dry=%lu\n",
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
        // Both controllers' edits: a client on the server's schedule corrects
        // the same drift, steering on time instead of depth.
        (unsigned long)(driftCtl.inserted + srvCtl.inserted + syncCtl.inserted),
        (unsigned long)(driftCtl.dropped + srvCtl.dropped + syncCtl.dropped),
        // Corrections per second the controller is currently asking for. Once
        // the loop is closed this is the only in-band measure of drift left:
        // a corrected buffer no longer has a slope to regress.
        outSynced ? syncCtl.rate() : levelCtl().rate(),
        // The level being steered to. It is measured after the settle window
        // rather than computed, so recording it is the only way to tell a
        // correctly calibrated client from one steering at the wrong depth.
        levelCtl().target(),
        // Age of the last received packet, as the silence check sees it. Signed
        // and printed even when it is meaningless (a source has no rx), because
        // a value that goes *negative* or jumps is the evidence that the check
        // is misfiring rather than the radio going quiet.
        lastRxMs ? (long)(millis() - lastRxMs) : -1L,
        // Largest allocatable block. Free heap can sit perfectly still while
        // this one falls, which is exactly what heap fragmentation looks like.
        (unsigned long)ESP.getMaxAllocHeap(),
        // Blocks that played only because their repeat copy arrived.
        (unsigned long)rxRecovered,
        // On the server's schedule, how far off it (us, + = this node late),
        // how often it had to jump back onto it, and how often the DMA ran dry
        // under a playing ring (lib/sync).
        outSynced ? 1 : 0, (long)syncErrUs, (unsigned long)syncJumps,
        (unsigned long)outDry);
}

static void benchIdentify() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    DEBUG_SERIAL.printf(
        "[BENCH] id fw=%s chip=%s psram=%lu mac=%02X:%02X:%02X:%02X:%02X:%02X "
        "bench=%d bt=%d espnow=%d drift=%d conly=%d mono=%d mute=%d trim=%d mesh=%04X meshname=%s "
        "name=%s link=%d cfg=%d\n",
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
        outTrimDb,
        meshId,
        meshNameForLog(),
        roomName,
        // D18: 2 a front end, 1 a node hearing one, 0 neither.
        frontEnd ? 2 : (linkFe ? 1 : 0),
        cfgOpen ? 1 : 0);
}

/**
 * `N`: where this node sits in the mesh, on one line that fits one reply
 * packet. `@* N` asks every node at once, and tools/mesh.ps1 draws the answers
 * as the star the mesh is: `src=` is a server's source (phone, bench) and a
 * client's server, by MAC, and rx/lost/und are what hearing it costs in the
 * current stream. No signal strength: the receive callback on this core (IDF
 * 4.4) has none, and loss is the measure the audio cares about anyway.
 */
static void benchNode() {
    const char *modeStr = currentMode == MODE_DISCOVERY ? "DISCOVERY"
                        : currentMode == MODE_SERVER    ? "SERVER"
                                                        : "CLIENT";
    char src[18] = "-";
    if (benchSource) {
        strcpy(src, "bench");
    } else if (currentMode == MODE_SERVER) {
        strcpy(src, linkSourceAllowed() ? "link" : "phone");   // a front end's phone (D18)
    } else if (currentMode == MODE_CLIENT && senderLocked) {
        snprintf(src, sizeof(src), "%02X:%02X:%02X:%02X:%02X:%02X", lockedSender[0],
                 lockedSender[1], lockedSender[2], lockedSender[3], lockedSender[4],
                 lockedSender[5]);
    }
    // bt: whether a phone could find this node now -- since D17 a node that
    // has been a client has Bluetooth off until its button is pressed. A
    // front end's is always on (D18).
    int btOn = (linkSourceAllowed() && linkFe) ? 1 : 0;
#ifdef ENABLE_BLUETOOTH
    if (btSinkStarted) btOn = 1;
#endif
    DEBUG_SERIAL.printf("[NODE] name=%s mac=%s fw=%s mode=%s src=%s rx=%lu lost=%lu und=%lu "
                        "up=%lus trim=%d mute=%d bt=%d mono=%d\n",
                        roomName, ownMac, FW_VERSION, modeStr, src, (unsigned long)rxCount,
                        (unsigned long)seqTracker.lost, (unsigned long)rxUnderrun,
                        (unsigned long)(millis() / 1000), outTrimDb, outputMuted ? 1 : 0, btOn,
                        clientMonoOut ? 1 : 0);
}

// ============================================================================
// Commands over the mesh (D16)
// ============================================================================
//
// `@<target> <command>` runs any serial command on another node of this mesh,
// or on all of them with `*`: the same dispatcher, commandRun(), whether the
// command came from this node's port or off the air, so a command added there
// works over the mesh with nothing more. What the command printed comes back
// (Console) and the asking node prints it as `[@<name>] <line>`. The packets
// are in lib/mesh, the transport in the control traffic section above.

static_assert(sizeof(ROOM_NAME) - 1 <= MESH_TARGET_MAX,
              "a room name longer than a target could never be asked for, or answer");

static void commandRun(char c, const char *arg, bool remote);

/**
 * Run a command with what it prints captured into `out`. `quiet` keeps the
 * output off this node's own port -- for the asking node's own share of an
 * `@*`, which it prints as a reply like everyone else's instead.
 */
static size_t commandCapture(const char *text, bool remote, bool quiet, char *out, size_t size) {
    const Console::Saved prev = debugOut.capture(out, size, quiet);
    commandRun(text[0], text + 1, remote);
    return debugOut.endCapture(prev);
}

/** A reply's text as `[@name] line`, a line at a time; `[@name]+` continues a line cut short. */
static void meshPrintReply(const char *name, size_t nameLen, bool cont, const char *text,
                           size_t len) {
    if (len == 0) {
        DEBUG_SERIAL.printf("[@%.*s] (done)\n", (int)nameLen, name);
        return;
    }
    for (size_t i = 0; i < len;) {
        size_t end = i;
        while (end < len && text[end] != '\n') end++;
        const size_t next = end + 1;
        if (end > i && text[end - 1] == '\r') end--;
        DEBUG_SERIAL.printf("[@%.*s]%s %.*s\n", (int)nameLen, name, (i == 0 && cont) ? "+" : "",
                            (int)(end - i), text + i);
        i = next;
    }
}

/** What a command printed, back to the node that asked: as many parts as it takes. */
static void ctrlReply(uint16_t id, const char *text, size_t len) {
    const size_t room = ESPNOW_PAYLOAD_SIZE - MESH_REPLY_HEADER(strlen(roomName));
    uint8_t payload[ESPNOW_PAYLOAD_SIZE];
    size_t  off  = 0;
    uint8_t part = 0;
    bool    cont = false;
    do {
        const size_t n    = meshReplyChunk(text + off, len - off, room);
        const size_t plen = meshPackReply(payload, sizeof(payload), id,
                                          (uint8_t)(part | (cont ? MESH_REPLY_CONT : 0)),
                                          roomName, text + off, n);
        if (plen == 0 || !ctrlQueue(MESH_REPLY_SEQ, payload, plen)) break;
        cont = n > 0 && text[off + n - 1] != '\n';
        off += n;
        part++;
    } while (off < len);
}

/**
 * `@<target> <command>`. The command goes out MESH_COMMAND_COPIES times, and
 * this node prints the answers for MESH_REPLY_WAIT_MS, then `[CMD] done` with
 * how many nodes answered. When this node is one of those asked -- `*`, or its
 * own name -- it runs the command too and prints its answer the same way.
 */
static void commandAskOnce(const char *line);

static void commandAsk(const char *line) {
    strncpy(cmdAskLine, line, sizeof(cmdAskLine) - 1);
    cmdAskLine[sizeof(cmdAskLine) - 1] = '\0';
    cmdAskRetries = MESH_ASK_RETRIES;
    commandAskOnce(cmdAskLine);
}

/** One asking of `@`'s question; commandAsk() and its retries call it. */
static void commandAskOnce(const char *line) {
    const char *target, *text;
    size_t      tlen, clen;
    if (!meshSplitCommandLine(line, &target, &tlen, &text, &clen)) {
        DEBUG_SERIAL.println("[CMD] error reason=usage (@<name|mac|*> <command>)");
        return;
    }
    char cmd[MESH_COMMAND_MAX + 1];
    memcpy(cmd, text, clen);
    cmd[clen] = '\0';

    const bool all = tlen == 1 && target[0] == MESH_TARGET_ALL[0];
    if (!meshCommandAllowedRemote(cmd[0])) {
        DEBUG_SERIAL.printf("[CMD] error reason=local_only cmd=%c\n", cmd[0]);
        return;
    }
    if (all && !meshCommandAllowedForAll(cmd[0])) {
        DEBUG_SERIAL.printf("[CMD] error reason=not_for_all cmd=%c\n", cmd[0]);
        return;
    }

    const bool self = all || meshTargetMatches(target, tlen, roomName, ownMac);
    cmdAskAll = all;
    if (all || self || !meshCommandRetryable(cmd)) cmdAskRetries = 0;
    if (all || !self) {
        if (!espnowActive || txQueue == nullptr) {
            DEBUG_SERIAL.println("[CMD] error reason=no_mesh");
            return;
        }
        if (replyQueue == nullptr) replyQueue = xQueueCreate(MESH_REPLY_QUEUE_DEPTH, sizeof(ReplyItem));
        if (replyQueue == nullptr) {
            DEBUG_SERIAL.println("[CMD] error reason=no_memory");
            return;
        }
        uint16_t id;
        do { id = (uint16_t)esp_random(); } while (id == 0);
        uint8_t      payload[sizeof(ctrlOut.payload)];
        const size_t n = meshPackCommand(payload, sizeof(payload), id, target, tlen, cmd, clen);

        cmdAskId = 0;                 // stop the callback queueing the last ask's
        xQueueReset(replyQueue);
        cmdAskReplies   = 0;
        cmdAskSeenCount = 0;
        cmdAskUntilMs = millis() + MESH_REPLY_WAIT_MS;
        cmdAskId      = id;
        ctrlSend(MESH_COMMAND_SEQ, payload, n, MESH_COMMAND_COPIES, MESH_COMMAND_INTERVAL_MS);
        DEBUG_SERIAL.printf("[CMD] ask id=%04X target=%.*s cmd=%s mesh=%04X\n", id, (int)tlen,
                            target, cmd, meshId);
    }

    if (self) {
        char         out[MESH_REPLY_CAPTURE];
        const size_t n = commandCapture(cmd, false, true, out, sizeof(out));
        meshPrintReply(roomName, strlen(roomName), false, out, n);
        if (all) cmdAskReplies++;
        else     DEBUG_SERIAL.println("[CMD] done replies=1");
    }
}

/**
 * Every side of the control traffic, from loop() in every mode: the request
 * this node is repeating, the command waiting for it, the replies to its own,
 * and an update request that named it.
 */
static void meshServiceControl() {
    if (otaRequested) {
        otaRequested = false;
        DEBUG_SERIAL.println("[OTA] update requested over the mesh");
        otaRebootIntoUpdate();
    }

    if (ctrlOut.left != 0 &&
        (ctrlOut.lastMs == 0 || (long)(millis() - ctrlOut.lastMs) >= (long)ctrlOut.intervalMs)) {
        ctrlOut.lastMs = millis();
        ctrlQueue(ctrlOut.seq, ctrlOut.payload, ctrlOut.len);
        if (--ctrlOut.left == 0 && ctrlOut.seq == MESH_UPDATE_SEQ) {
            DEBUG_SERIAL.println("[OTA] request sent");
        }
    }

    if (ctrlIn.ready && (long)(millis() - ctrlIn.runAtMs) >= 0) {
        char text[MESH_COMMAND_MAX + 1];
        memcpy(text, ctrlIn.text, sizeof(text));
        const uint16_t id = ctrlIn.id;
        DEBUG_SERIAL.printf("[CMD] from %02X:%02X:%02X:%02X:%02X:%02X id=%04X: %s\n",
                            ctrlIn.from[0], ctrlIn.from[1], ctrlIn.from[2], ctrlIn.from[3],
                            ctrlIn.from[4], ctrlIn.from[5], id, text);
        ctrlIn.ready = false;

        char         out[MESH_REPLY_CAPTURE];
        const size_t n = commandCapture(text, true, false, out, sizeof(out));
        ctrlReply(id, out, n);

        // Kept for the copies still on their way (ctrlLast).
        free(ctrlLast.text);
        ctrlLast.text   = (char *)malloc(n > 0 ? n : 1);
        ctrlLast.len    = n;
        ctrlLast.resend = false;
        ctrlLast.untilMs = millis() + MESH_COMMAND_COPIES * MESH_COMMAND_INTERVAL_MS;
        if (ctrlLast.text != nullptr) memcpy(ctrlLast.text, out, n);
        ctrlLast.id = id;
    }

    if (ctrlLast.text != nullptr) {
        if (ctrlLast.resend && (long)(millis() - ctrlLast.resendAtMs) >= 0) {
            ctrlLast.resend = false;
            ctrlReply(ctrlLast.id, ctrlLast.text, ctrlLast.len);
        } else if (!ctrlLast.resend && (long)(millis() - ctrlLast.untilMs) >= 0) {
            ctrlLast.id = 0;
            free(ctrlLast.text);
            ctrlLast.text = nullptr;
        }
    }

    if (replyQueue != nullptr) {
        ReplyItem item;
        while (xQueueReceive(replyQueue, &item, 0) == pdTRUE) {
            MeshReply r;
            if (!meshUnpackReply(item.data, item.len, &r)) continue;
            const uint32_t key  = meshReplyKey(r.name, r.nameLen, r.part);
            bool           seen = false;
            for (uint8_t i = 0; i < cmdAskSeenCount; i++) seen |= cmdAskSeen[i] == key;
            if (seen) continue;
            if (cmdAskSeenCount < sizeof(cmdAskSeen) / sizeof(cmdAskSeen[0])) {
                cmdAskSeen[cmdAskSeenCount++] = key;
            }
            if ((r.part & ~MESH_REPLY_CONT) == 0) {
                cmdAskReplies++;
                // One node asked, and it has begun to answer: the rest of its
                // parts are a few ms behind, not MESH_REPLY_WAIT_MS.
                const unsigned long settle = millis() + MESH_REPLY_SETTLE_MS;
                if (!cmdAskAll && (long)(cmdAskUntilMs - settle) > 0) cmdAskUntilMs = settle;
            }
            meshPrintReply(r.name, r.nameLen, (r.part & MESH_REPLY_CONT) != 0, r.text, r.textLen);
        }
    }
    if (cmdAskId != 0 && (long)(millis() - cmdAskUntilMs) >= 0) {
        const uint16_t id = cmdAskId;
        cmdAskId = 0;
        if (cmdAskReplies == 0 && cmdAskRetries > 0) {
            // Nobody answered. The question is one that means the same twice,
            // so ask again rather than leave the asker to guess (D16, D19).
            cmdAskRetries--;
            DEBUG_SERIAL.printf("[CMD] retry id=%04X left=%u\n", id, cmdAskRetries);
            commandAskOnce(cmdAskLine);
        } else {
            DEBUG_SERIAL.printf("[CMD] done id=%04X replies=%u\n", id, cmdAskReplies);
        }
    }
}

// ============================================================================
// The link, from loop() (D18)
// ============================================================================

/** The front end's answer, a line at a time as `[FE] <line>`, if one is waiting. */
static void linkPrintReply() {
    if (!linkReplyReady) return;
    char   text[LINK_MAX_PAYLOAD + 1];
    size_t len;
    portENTER_CRITICAL(&linkMsgMux);
    len = linkReplyLen;
    memcpy(text, linkReply, len + 1);
    linkReplyReady = false;
    portEXIT_CRITICAL(&linkMsgMux);
    for (size_t i = 0; i < len;) {
        size_t end = i;
        while (end < len && text[end] != '\n') end++;
        const size_t next = end + 1;
        if (end > i && text[end - 1] == '\r') end--;
        if (end > i) DEBUG_SERIAL.printf("[FE] %.*s\n", (int)(end - i), text + i);
        i = next;
    }
}

/**
 * `><command>`: run a command on this node's front end, and print its answer.
 *
 * It waits for the answer, so that the answer is part of this command's output
 * -- which is what carries it back over the mesh to whoever asked (D16) -- and
 * while it waits it keeps this node's speaker fed, as loop() would.
 */
static void linkAsk(const char *line) {
    if (frontEnd || !linkUp) {
        DEBUG_SERIAL.println("[LINK] error reason=no_link");
        return;
    }
    if (!linkFe) {
        DEBUG_SERIAL.println("[LINK] error reason=no_front_end (nothing heard on the link)");
        return;
    }
    // The mesh's rules, for the same reasons: `W` would put the password on
    // the wire, and a front end does not relay.
    if (line[0] == '>' || !meshCommandAllowedRemote(line[0])) {
        DEBUG_SERIAL.println("[LINK] error reason=local_only");
        return;
    }
    portENTER_CRITICAL(&linkMsgMux);
    linkReplyReady = false;
    portEXIT_CRITICAL(&linkMsgMux);
    if (!linkSend(LINK_CMD, (const uint8_t *)line, strnlen(line, MESH_COMMAND_MAX), false)) {
        DEBUG_SERIAL.println("[LINK] error reason=send");
        return;
    }
    const unsigned long t0 = millis();
    while (!linkReplyReady && (long)(millis() - t0) < (long)LINK_REPLY_WAIT_MS) {
        if (outI2SActive && currentMode != MODE_DISCOVERY) driveRingI2S();
        else delay(1);
    }
    if (!linkReplyReady) {
        DEBUG_SERIAL.println("[LINK] error reason=no_answer");
        return;
    }
    linkPrintReply();
}

/** The node's side, every pass: a front end that went quiet, and late answers. */
static void linkServiceNode() {
    if (!linkUp || frontEnd) return;
    linkPrintReply();
    if (!linkFe || (long)(millis() - linkFeMs) < (long)LINK_LOST_MS) return;
    linkFe = false;
    DEBUG_SERIAL.println("[LINK] front end silent: lost");
    if (linkSourceAllowed() && btConnected) {
        btConnected       = false;
        txReady           = false;
        doDisconnectSound = true;
    }
}

// The front end's test signal (`s`): the bench tone, paced as A2DP from
// Windows arrives -- a packet of LINK_TONE_PACKET_FRAMES at a time.
static bool     linkTone      = false;
static int64_t  linkToneStart = 0;
static uint32_t linkTonePkt   = 0;

static void linkToneSet(bool on) {
    if (on && !benchToneReady) benchBuildTone();
    linkTone       = on;
    linkToneStart  = esp_timer_get_time();
    linkTonePkt    = 0;
    benchTonePhase = 0;
    linkStateDirty = true;
    DEBUG_SERIAL.printf("[LINK] tone=%d\n", on ? 1 : 0);
}

static void linkServiceTone() {
    if (!linkTone) return;
    const int64_t now = esp_timer_get_time();
    for (int burst = 0; burst < BENCH_MAX_CATCHUP_PKTS; burst++) {
        const int64_t due = linkToneStart + (int64_t)linkTonePkt * 1000000LL *
                                                LINK_TONE_PACKET_FRAMES / CLIENT_SAMPLE_RATE;
        if (now < due) return;
        int16_t pcm[2 * LINK_AUDIO_CHUNK];
        for (int done = 0; done < LINK_TONE_PACKET_FRAMES; done += LINK_AUDIO_CHUNK) {
            for (int i = 0; i < LINK_AUDIO_CHUNK; i++) {
                const int16_t s = benchTone[benchTonePhase];
                if (++benchTonePhase >= BENCH_TONE_LEN) benchTonePhase = 0;
                pcm[2 * i] = pcm[2 * i + 1] = s;
            }
            linkSend(LINK_AUDIO, (const uint8_t *)pcm, sizeof(pcm), true);
        }
        linkTonePkt++;
    }
}

/** The front end's loop(): commands from its node, the tone, and its state. */
static void linkServiceFrontEnd() {
    if (linkCmdReady) {
        char text[MESH_COMMAND_MAX + 1];
        portENTER_CRITICAL(&linkMsgMux);
        memcpy(text, linkCmd, sizeof(text));
        linkCmdReady = false;
        portEXIT_CRITICAL(&linkMsgMux);
        char         out[MESH_REPLY_CAPTURE];
        const size_t n = commandCapture(text, true, false, out, sizeof(out));
        linkSend(LINK_REPLY, (const uint8_t *)out, n, false);
    }

    linkServiceTone();

#ifdef ENABLE_BLUETOOTH
    LinkState s;
    s.connected  = btConnected || linkTone;
    s.streaming  = txReady || linkTone;
    s.sampleRate = linkTone ? CLIENT_SAMPLE_RATE : a2dpSink.sample_rate();
    s.volume     = (uint8_t)a2dpSink.get_volume();
    static LinkState     sent;
    static unsigned long sentMs = 0;
    const bool changed = s.connected != sent.connected || s.streaming != sent.streaming ||
                         s.sampleRate != sent.sampleRate || s.volume != sent.volume;
    if (changed || linkStateDirty || sentMs == 0 ||
        (long)(millis() - sentMs) >= (long)LINK_STATE_INTERVAL_MS) {
        uint8_t      b[LINK_STATE_BYTES];
        const size_t n = linkPackState(s, b, sizeof(b));
        if (linkSend(LINK_STATE, b, n, false)) {
            sent           = s;
            sentMs         = millis();
            linkStateDirty = false;
        }
    }
#endif

#if DEBUG_LEVEL >= 3
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > STATUS_INTERVAL_MS) {
        lastStatus = millis();
        LOG_INFO(String("Status: mode=FRONTEND heap=") + String(ESP.getFreeHeap()) +
                 " phone=" + String(btConnected ? 1 : 0) + " audio=" + String(txReady ? 1 : 0) +
                 " tone=" + String(linkTone ? 1 : 0) + " tx=" + String(linkTxFrames) +
                 " drops=" + String(linkTxDrops) + " rx=" + String(linkDec->frames) +
                 " crc=" + String(linkDec->crcErrors));
    }
#endif
}

// ============================================================================
// The setup network (D19)
// ============================================================================
//
// An access point on the mesh's own channel, with a page that lists the mesh
// and sets each node. The node stays on the mesh while it is open -- AP and
// ESP-NOW share the one radio and the one channel -- and keeps playing, unless
// its own chip runs Bluetooth: Espressif rates a SoftAP beside Bluetooth
// unstable, and a WROVER has the RAM for one of them, so Bluetooth is paused
// for as long as the network is open, and the network is refused to a node
// serving a phone with it. A two-chip node (D18) has no such problem.
//
// The page knows nothing the commands do not: every change is `@<mac> <cmd>`,
// run by loop() exactly as if typed on this node's port, and what it shows is
// what the node answered (meshCommandRetryable, MESH_ASK_RETRIES).

static unsigned long  cfgUntilMs    = 0;     // the window...
static volatile unsigned long cfgLastReqMs = 0;   // ...held open by the page's requests
static bool           cfgSecured    = false; // the network as it is running: WPA2, or open
static esp_netif_t   *cfgApNetif    = nullptr;
static httpd_handle_t cfgHttp       = nullptr;
static volatile bool  cfgDnsRun     = false;
static volatile bool  cfgDnsAlive   = false;   // its task has not ended yet
static volatile unsigned long cfgReopenAtMs = 0;   // the password changed: restart the AP with it
static volatile unsigned long cfgCloseAtMs  = 0;   // the page said close
static char           cfgApPass[CONFIG_AP_PASS_MAX + 1] = "";

// A command from the page, run by loop(). One at a time: the HTTP task queues
// it and waits, loop() runs it and captures what it prints -- across as many
// passes as a question to the mesh takes to be answered -- and hands it back.
// A request that gave up waiting leaves its command ABANDONED, and loop()
// frees the slot when it finishes.
enum CfgCmdState : uint8_t { CFG_IDLE, CFG_QUEUED, CFG_RUNNING, CFG_DONE, CFG_ABANDONED };
static portMUX_TYPE           cfgMux       = portMUX_INITIALIZER_UNLOCKED;
static volatile CfgCmdState   cfgCmdState  = CFG_IDLE;
static char                   cfgCmdLine[2 + MESH_TARGET_MAX + MESH_COMMAND_MAX];
static char                  *cfgCmdOut    = nullptr;   // CONFIG_REPLY_MAX, from the heap
static size_t                 cfgCmdLen    = 0;
static Console::Saved         cfgCmdPrev;

static void cfgApStart() {
    wifi_config_t ap = {};
    // The room name, so a phone's list says which box it is joining.
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s-setup", roomName);
    ap.ap.ssid_len       = (uint8_t)strlen((const char *)ap.ap.ssid);
    ap.ap.channel        = ESPNOW_CHANNEL;
    ap.ap.max_connection = CONFIG_MAX_CLIENTS;
    ap.ap.beacon_interval = 100;
    cfgSecured = cfgApPass[0] != '\0';
    if (cfgSecured) {
        strlcpy((char *)ap.ap.password, cfgApPass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    // The STA side, which ESP-NOW sends on, stays on the mesh's channel with it.
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

/**
 * Every name a phone looks up is this node: what makes the phone notice that
 * it is on a captive network and open the page by itself. A question for an
 * address of another kind (AAAA) gets an empty answer, so the phone asks for
 * an IPv4 one instead of waiting.
 */
static void cfgDnsTask(void *) {
    cfgDnsAlive = true;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    timeval tv = {0, 300000};   // to notice cfgDnsRun going false
    if (sock < 0 || bind(sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
        DEBUG_SERIAL.println("[CONFIG] error reason=dns");
        if (sock >= 0) close(sock);
        cfgDnsAlive = false;
        vTaskDelete(nullptr);
        return;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(cfgApNetif, &ip);
    uint8_t buf[512];
    while (cfgDnsRun) {
        sockaddr_in from;
        socklen_t   flen = sizeof(from);
        const int   n    = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr *)&from, &flen);
        // A standard query (QR 0, opcode 0) with exactly one question.
        if (n < 12 || (buf[2] & 0xF8) != 0 || buf[4] != 0 || buf[5] != 1) continue;
        int i = 12;
        while (i < n && buf[i] != 0) i += buf[i] + 1;
        i += 5;   // the name's terminator, then type and class
        if (i > n || i + 16 > (int)sizeof(buf)) continue;
        const bool a = buf[i - 4] == 0 && buf[i - 3] == 1;
        buf[2] = (uint8_t)(0x84 | (buf[2] & 0x01));   // an authoritative answer, RD kept
        buf[3] = 0x80;
        buf[6] = 0;
        buf[7] = a ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;   // and nothing else, EDNS included
        if (a) {
            const uint8_t rr[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            memcpy(buf + i, rr, sizeof(rr));
            memcpy(buf + i + sizeof(rr), &ip.ip.addr, 4);
            i += sizeof(rr) + 4;
        }
        sendto(sock, buf, i, 0, (sockaddr *)&from, flen);
    }
    close(sock);
    cfgDnsAlive = false;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------- HTTP

static void cfgTouch() { cfgLastReqMs = millis(); }

/** A small request body, terminated. False if it is longer than `size` - 1. */
static bool cfgBody(httpd_req_t *req, char *out, size_t size) {
    if (req->content_len >= size) return false;
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, out + got, req->content_len - got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    out[got] = '\0';
    return true;
}

static esp_err_t cfgSend(httpd_req_t *req, const char *status, const char *text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

/** Into a JSON string: names are checked to need nothing (meshRoomNameValid), a mesh name may. */
static size_t cfgJsonStr(char *out, size_t size, const char *s) {
    size_t n = 0;
    for (; *s && n + 3 < size; s++) {
        const char c = *s;
        if (c == '"' || c == '\\') out[n++] = '\\';
        out[n++] = ((unsigned char)c < ' ') ? '?' : c;
    }
    out[n] = '\0';
    return n;
}

static esp_err_t cfgHttpInfo(httpd_req_t *req) {
    cfgTouch();
    char mesh[2 * MESH_NAME_MAX + 2], ssid[2 * 32 + 2];
    cfgJsonStr(mesh, sizeof(mesh), meshNameForLog());
    Preferences nvs;   // its own: `prefs` belongs to loop()
    nvs.begin(PREF_NAMESPACE, true);
    const String stored = nvs.getString(PREF_WIFI_SSID, "");
    nvs.end();
    cfgJsonStr(ssid, sizeof(ssid), stored.c_str());
    char out[384];
    snprintf(out, sizeof(out),
             "{\"name\":\"%s\",\"mac\":\"%s\",\"fw\":\"%s\",\"mesh\":\"%04X\",\"meshname\":\"%s\","
             "\"protected\":%s,\"wifi\":\"%s\"}",
             roomName, ownMac, FW_VERSION, meshId, mesh, cfgSecured ? "true" : "false", ssid);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t cfgHttpCmd(httpd_req_t *req) {
    cfgTouch();
    char line[sizeof(cfgCmdLine)];
    if (!cfgBody(req, line, sizeof(line))) return cfgSend(req, "400 Bad Request", "too long\n");
    // Only questions to the mesh: `@<target> <command>`, whose rules (`W`
    // never travels, nothing that silences the house goes to `*`) commandAsk
    // already enforces.
    if (line[0] != '@') return cfgSend(req, "400 Bad Request", "@<target> <command> only\n");

    portENTER_CRITICAL(&cfgMux);
    const bool free = cfgCmdState == CFG_IDLE;
    if (free) {
        memcpy(cfgCmdLine, line, sizeof(cfgCmdLine));
        cfgCmdState = CFG_QUEUED;
    }
    portEXIT_CRITICAL(&cfgMux);
    if (!free) return cfgSend(req, "503 Service Unavailable", "busy: one question at a time\n");

    const unsigned long t0 = millis();
    while ((long)(millis() - t0) < (long)CONFIG_CMD_TIMEOUT_MS) {
        if (cfgCmdState == CFG_DONE) {
            httpd_resp_set_type(req, "text/plain");
            const esp_err_t err = httpd_resp_send(req, cfgCmdOut, cfgCmdLen);
            portENTER_CRITICAL(&cfgMux);
            cfgCmdState = CFG_IDLE;
            portEXIT_CRITICAL(&cfgMux);
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Given up. Not yet started, or already done: the slot is free again.
    // Running: loop() frees it when it finishes, having a capture to end.
    portENTER_CRITICAL(&cfgMux);
    cfgCmdState = cfgCmdState == CFG_RUNNING ? CFG_ABANDONED : CFG_IDLE;
    portEXIT_CRITICAL(&cfgMux);
    return cfgSend(req, "504 Gateway Timeout", "no answer in time\n");
}

static esp_err_t cfgHttpApPass(httpd_req_t *req) {
    cfgTouch();
    char pass[CONFIG_AP_PASS_MAX + 2];
    if (!cfgBody(req, pass, sizeof(pass))) return cfgSend(req, "400 Bad Request", "at most 63 characters\n");
    const size_t len = strlen(pass);
    bool ok = len >= CONFIG_AP_PASS_MIN && len <= CONFIG_AP_PASS_MAX;
    for (size_t i = 0; ok && i < len; i++) ok = pass[i] >= ' ' && pass[i] <= '~';
    if (!ok) return cfgSend(req, "400 Bad Request", "8 to 63 printable characters\n");
    Preferences nvs;
    nvs.begin(PREF_NAMESPACE, false);
    nvs.putString(PREF_AP_PASS, pass);
    nvs.end();
    strlcpy(cfgApPass, pass, sizeof(cfgApPass));
    memset(pass, 0, sizeof(pass));
    cfgReopenAtMs = millis() + 1500;   // after this answer has gone
    DEBUG_SERIAL.println("[CONFIG] password set: restarting the network with it");
    return cfgSend(req, "200 OK", "Saved. The network restarts in a moment: join it again with the new password.\n");
}

static esp_err_t cfgHttpWifi(httpd_req_t *req) {
    cfgTouch();
    // Only over a network with a password: on an open one the home WiFi's
    // password would cross the air for anyone to read.
    if (!cfgSecured) return cfgSend(req, "403 Forbidden", "set a password for this network first\n");
    char body[33 + 65 + 2];
    if (!cfgBody(req, body, sizeof(body))) return cfgSend(req, "400 Bad Request", "too long\n");
    char *nl = strchr(body, '\n');
    if (nl == nullptr || nl == body || nl - body > 32 || strlen(nl + 1) > 64) {
        return cfgSend(req, "400 Bad Request", "a network name of 1-32 characters, then its password\n");
    }
    *nl = '\0';
    Preferences nvs;
    nvs.begin(PREF_NAMESPACE, false);
    nvs.putString(PREF_WIFI_SSID, body);
    nvs.putString(PREF_WIFI_PASS, nl + 1);
    nvs.end();
    memset(body, 0, sizeof(body));
    DEBUG_SERIAL.println("[CONFIG] home wifi stored");
    return cfgSend(req, "200 OK", "Saved for update mode.\n");
}

static esp_err_t cfgHttpClose(httpd_req_t *req) {
    cfgCloseAtMs = millis() + 500;
    return cfgSend(req, "200 OK", "closing\n");
}

/** The page at `/`; any other address -- a phone checking for internet -- is sent to it. */
static esp_err_t cfgHttpPage(httpd_req_t *req) {
    cfgTouch();
    if (strcmp(req->uri, "/") == 0 || strncmp(req->uri, "/?", 2) == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, SETUP_PAGE, sizeof(SETUP_PAGE) - 1);
    }
    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(cfgApNetif, &ip);
    char to[32];
    snprintf(to, sizeof(to), "http://" IPSTR "/", IP2STR(&ip.ip));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", to);
    return httpd_resp_send(req, nullptr, 0);
}

// ---------------------------------------------------------------- open and close

static void configClose(const char *why);

/** Open the setup network for `windowMs`, or keep it open that long. `C`, and the long hold. */
static void configOpen(uint32_t windowMs) {
    if (cfgOpen) {
        cfgUntilMs = millis() + windowMs;
        DEBUG_SERIAL.printf("[CONFIG] open, for %lus more\n", (unsigned long)(windowMs / 1000));
        return;
    }
    if (frontEnd) {
        DEBUG_SERIAL.println("[CONFIG] error reason=front_end (no WiFi here; open it on the node)");
        return;
    }
    if (benchMode) {
        DEBUG_SERIAL.println("[CONFIG] error reason=bench");
        return;
    }
    if (!espnowActive) {
        DEBUG_SERIAL.println("[CONFIG] error reason=no_wifi (a WROOM speaker; client-only mode has it)");
        return;
    }
#ifdef ENABLE_BLUETOOTH
    if (ownBt() && currentMode == MODE_SERVER) {
        DEBUG_SERIAL.println("[CONFIG] error reason=serving (a phone is on this chip's Bluetooth)");
        return;
    }
    if (ownBt() && btSinkStarted) {
        btWindow = false;
        stopBluetooth();
        cfgPausedBt = true;
    }
#endif
    if (cfgCmdOut == nullptr) cfgCmdOut = (char *)malloc(CONFIG_REPLY_MAX);
    if (cfgApNetif == nullptr) cfgApNetif = esp_netif_create_default_wifi_ap();
    if (cfgCmdOut == nullptr || cfgApNetif == nullptr) {
        DEBUG_SERIAL.println("[CONFIG] error reason=no_memory");
        return;
    }
    cfgApStart();

    httpd_config_t hc   = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn     = httpd_uri_match_wildcard;
    hc.max_open_sockets = 4;
    hc.lru_purge_enable = true;
    hc.stack_size       = 6144;
    if (httpd_start(&cfgHttp, &hc) != ESP_OK) {
        DEBUG_SERIAL.println("[CONFIG] error reason=http");
        cfgHttp = nullptr;
    } else {
        const httpd_uri_t routes[] = {
            {"/api/info",   HTTP_GET,  cfgHttpInfo,   nullptr},
            {"/api/cmd",    HTTP_POST, cfgHttpCmd,    nullptr},
            {"/api/appass", HTTP_POST, cfgHttpApPass, nullptr},
            {"/api/wifi",   HTTP_POST, cfgHttpWifi,   nullptr},
            {"/api/close",  HTTP_POST, cfgHttpClose,  nullptr},
            {"/*",          HTTP_GET,  cfgHttpPage,   nullptr},
        };
        for (const httpd_uri_t &r : routes) httpd_register_uri_handler(cfgHttp, &r);
    }
    for (int i = 0; cfgDnsAlive && i < 50; i++) delay(10);   // a window just closed
    cfgDnsRun = true;
    xTaskCreate(cfgDnsTask, "cfg_dns", 3072, nullptr, 3, nullptr);

    cfgOpen       = true;
    cfgUntilMs    = millis() + windowMs;
    cfgLastReqMs  = millis();
    cfgCloseAtMs  = 0;
    cfgReopenAtMs = 0;
    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(cfgApNetif, &ip);
    DEBUG_SERIAL.printf("[CONFIG] open ssid=%s-setup auth=%s ip=" IPSTR " for=%lus bt_paused=%d\n",
                        roomName, cfgSecured ? "wpa2" : "open", IP2STR(&ip.ip),
                        (unsigned long)(windowMs / 1000), cfgPausedBt ? 1 : 0);
    meshPlayFeedback(MESH_FB_OPEN);   // two drops, as every window opening
}

static void configClose(const char *why) {
    if (!cfgOpen) return;
    if (cfgCmdState != CFG_IDLE) {
        // The page is waiting for a command, which only loop() -- this caller
        // -- can finish, and httpd_stop() would wait for the page: close on a
        // later pass instead.
        cfgCloseAtMs = millis() + 100;
        DEBUG_SERIAL.println("[CONFIG] closing after the page's command");
        return;
    }
    cfgDnsRun = false;   // its task ends at its next receive timeout
    if (cfgHttp != nullptr) {
        httpd_stop(cfgHttp);
        cfgHttp = nullptr;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    cfgOpen = false;
    // A command the page was waiting for is left to finish and free its slot.
    DEBUG_SERIAL.printf("[CONFIG] closed (%s)\n", why);
#ifdef ENABLE_BLUETOOTH
    if (cfgPausedBt) {
        // Back as it was -- unless the node joined a stream meanwhile, where
        // D17 keeps Bluetooth off until somebody presses for it.
        cfgPausedBt = false;
        if (currentMode != MODE_CLIENT) startBluetooth();
    }
#endif
    meshPlayFeedback(MESH_FB_CLOSED);
}

/** From loop(): the page's commands, its password restart, and the window's end. */
static void configService() {
    // The page's command: start it, and finish it once no question it asked
    // is still waiting for the mesh.
    if (cfgCmdState == CFG_QUEUED) {
        portENTER_CRITICAL(&cfgMux);
        cfgCmdState = CFG_RUNNING;
        portEXIT_CRITICAL(&cfgMux);
        cfgCmdPrev = debugOut.capture(cfgCmdOut, CONFIG_REPLY_MAX, false);
        commandRun(cfgCmdLine[0], cfgCmdLine + 1, false);
    }
    if ((cfgCmdState == CFG_RUNNING || cfgCmdState == CFG_ABANDONED) && cmdAskId == 0) {
        cfgCmdLen = debugOut.endCapture(cfgCmdPrev);
        portENTER_CRITICAL(&cfgMux);
        cfgCmdState = cfgCmdState == CFG_ABANDONED ? CFG_IDLE : CFG_DONE;
        portEXIT_CRITICAL(&cfgMux);
    }

    if (!cfgOpen) return;
    if (cfgReopenAtMs != 0 && (long)(millis() - cfgReopenAtMs) >= 0) {
        cfgReopenAtMs = 0;
        cfgApStart();
        DEBUG_SERIAL.printf("[CONFIG] network restarted auth=%s\n", cfgSecured ? "wpa2" : "open");
    }
    if (cfgCloseAtMs != 0 && (long)(millis() - cfgCloseAtMs) >= 0) {
        cfgCloseAtMs = 0;
        configClose("closed from the page");
        return;
    }
    const unsigned long now = millis();
    if ((long)(now - cfgUntilMs) >= 0 && (long)(now - cfgLastReqMs) >= (long)CONFIG_IDLE_MS) {
        configClose("timed out");
    }
}

/**
 * The loss trace accumulated since the last call, once a second while `L1`:
 * `[LT] t=<ms> s=<seq> n=<count> drop=<total> <hex>`, a bit per packet from
 * s= on, LSB of each byte first, 1 = lost. A line whose s= does not follow
 * the last one's s= + n= is a break -- a resync, a new stream, or this falling
 * behind -- and drop= counts what breaks cost. tools/btlisten/losstrace.py
 * reads it.
 */
static void traceService() {
    static unsigned long lastMs = 0;
    if (!traceOn || (long)(millis() - lastMs) < (long)LOSS_TRACE_PRINT_MS) return;
    lastMs = millis();
    static const char HEX_DIGITS[] = "0123456789abcdef";
    uint8_t bits[LOSS_TRACE_LINE_PKTS / 8];
    char    hex[LOSS_TRACE_LINE_PKTS / 4 + 1];
    for (int lines = 0; lines < LossTrace::BITS / LOSS_TRACE_LINE_PKTS; lines++) {
        uint16_t start;
        portENTER_CRITICAL(&traceMux);
        const int      n       = rxTrace.take(&start, bits, LOSS_TRACE_LINE_PKTS);
        const uint32_t dropped = rxTrace.dropped;
        portEXIT_CRITICAL(&traceMux);
        if (n == 0) break;
        const int bytes = (n + 7) / 8;
        for (int i = 0; i < bytes; i++) {
            hex[2 * i]     = HEX_DIGITS[bits[i] >> 4];
            hex[2 * i + 1] = HEX_DIGITS[bits[i] & 0x0F];
        }
        hex[2 * bytes] = '\0';
        DEBUG_SERIAL.printf("[LT] t=%lu s=%u n=%d drop=%lu %s\n", (unsigned long)lastMs,
                            (unsigned)start, n, (unsigned long)dropped, hex);
        // Only a full line means more is waiting. Otherwise what arrived while
        // that line went out -- 10 ms at 115200 baud, four packets -- waits for
        // the next second instead of making a line of its own.
        if (n < LOSS_TRACE_LINE_PKTS) break;
    }
}

/**
 * Read the rest of a command line, for a command that takes an argument.
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
 * Commands: a letter, and for some an argument on the rest of the line
 * (commandTakesArg), so the host side needs no protocol. Every one of them
 * also runs on another node with `@<target>` in front (D16), except `W`.
 *
 *   ?  identify          b  reboot into bench mode     n  reboot into normal mode
 *   N  where this node sits in the mesh: mode, its source, what hearing it costs
 *   @  `@<name|mac|*> <command>` runs the command on that node, or on every
 *      node of the mesh, and prints their answers as `[@<name>] <line>`
 *   s  start sourcing    x  stop sourcing              r  report now
 *   d  toggle clock-drift correction
 *   c  toggle client-only mode and reboot (persists across power cuts)
 *   g  print the mesh identity; `g<name>` sets it (persists, no reboot)
 *   p  listen for an offer and join that mesh (the speaker half of pairing)
 *   o  offer this mesh to a node that is listening (the server half)
 *   B  Bluetooth on for BT_WINDOW_MS so a phone can connect: the short press (D17)
 *   m  mute this node's speaker until reboot (the mesh is untouched)
 *   M  toggle mixing a client's stereo to mono, for one speaker (persists)
 *   v  `v<dB>` this node's volume trim, applied at its own output (persists)
 *   l  client: print and reset the lost-run histogram and `rec`
 *   L  client: `L1` prints which packets were lost, once a second, `L0` stops
 *   z  client: `z1` fills a lost block from its neighbours, `z0` with silence
 *   t  `t<n>` sends each mesh frame n times, 1..3, until reboot
 *   P  `P<us>` minimum spacing between audio packets, 0 = none, until reboot
 *   D  `D<n>` each packet also carries the block n packets back, until reboot
 *   X  `X<n>` each packet carries the XOR of blocks 1 and n back instead
 *   R  `R<Mbps>` sets the ESP-NOW PHY rate this node sends at, until reboot
 *   U  bare: reboot into update mode. `U<name>`: ask the node with that
 *      ROOM_NAME or MAC to, over the mesh (see tools/ota.ps1)
 *   W  `W<ssid>` then the password on the next line: the home network update
 *      mode joins (persists). Bare `W` prints the SSID, never the password.
 *      This node's port only: over the mesh the password would be broadcast
 *   F  toggle front-end mode and reboot (persists): Bluetooth only, handed to
 *      a node over the link (D18). Classic boards only
 *   I  `I<name>` this node's name, the Bluetooth one too (persists); bare reports
 *   C  open the setup network for CONFIG_WINDOW_MS, `C0` closes it (D19)
 *   m, M  also `m1`/`m0` and `M1`/`M0`, which set rather than toggle
 *   >  `><command>` runs the command on this node's front end and prints its
 *      answer as `[FE] <line>`. On a front end, `s` and `x` start and stop the
 *      bench tone over the link instead of over the mesh
 *
 * Bluetooth server diagnostics, for telling the A2DP side from the mesh side:
 *   k  `k<aa:bb:cc:dd:ee:ff>` dials a bonded A2DP source, as a headset would
 *   V  `V<0..127>` sets the A2DP volume, which the mesh stream carries too
 *   e  `e<n>` coexistence preference: 0 WiFi, 1 Bluetooth, 2 balance
 *   a  print and reset the A2DP timing window (see a2dpDataCallback)
 *   f  toggle forwarding A2DP audio to the mesh; local playback continues
 *   w  stop WiFi altogether, until the next reboot -- what a WROOM server is
 *   j  play the connect jingle as a connection does -- during a stream too
 */

/**
 * Which commands take the rest of their line as an argument. The others are
 * one keystroke, and must stay one: reading a line blocks loop() for up to a
 * quarter second (benchReadLine), and bench-mesh.ps1 sends `?` mid-stream.
 */
static bool commandTakesArg(char c) {
    return c != '\0' && strchr("gzLtDXPRvUVek@>mMIC", c) != nullptr;
}

/**
 * `D<n>`: each packet carries the block n packets back; `X<n>`: the XOR of
 * blocks 1 and n back (n >= 2). Until reboot; 0 = none. Clients read both
 * off the wire, so this moves the whole mesh.
 */
static void benchSetRedundancy(bool parity, const char *arg) {
    const int n = atoi(arg);
    if (n >= 0 && n < MESH_TX_HISTORY && !(parity && n == 1)) {
        txDistance = (uint8_t)n;
        txParity   = parity;
    }
    DEBUG_SERIAL.printf("[MESH] distance=%u parity=%d\n", txDistance, txParity ? 1 : 0);
}

/**
 * `W<ssid>`, then the password on the next line. Two lines, so an SSID may
 * contain spaces; and on this node's port only, never over the mesh -- which
 * is why it is not in commandRun. The password is never printed back, only
 * whether there is one.
 */
static void benchSetWifi() {
    char ssid[33];
    benchReadLine(ssid, sizeof(ssid));
    prefs.begin(PREF_NAMESPACE, ssid[0] == '\0');
    if (ssid[0] != '\0') {
        char pass[65];
        benchReadLine(pass, sizeof(pass));
        prefs.putString(PREF_WIFI_SSID, ssid);
        prefs.putString(PREF_WIFI_PASS, pass);
        memset(pass, 0, sizeof(pass));
    }
    const String storedSsid = prefs.getString(PREF_WIFI_SSID, "");
    const bool   hasPass    = prefs.getString(PREF_WIFI_PASS, "").length() > 0;
    prefs.end();
    DEBUG_SERIAL.printf("[OTA] wifi ssid=%s pass=%s\n",
                        storedSsid.length() ? storedSsid.c_str() : "-",
                        hasPass ? "set" : "none");
}

/** Commands typed on this node's own port. */
static void benchServiceSerial() {
    while (DEBUG_SERIAL.available()) {
        const char c = (char)DEBUG_SERIAL.read();
        if (c == 'W') {
            benchSetWifi();
            continue;
        }
        char arg[3 + MESH_TARGET_MAX + MESH_COMMAND_MAX];   // `@`'s is the longest
        arg[0] = '\0';
        if (commandTakesArg(c)) benchReadLine(arg, sizeof(arg));
        commandRun(c, arg, false);
    }
}

/**
 * Run one command: typed on this node's port, or (`remote`) sent by another
 * node over the mesh -- the same code either way, which is what makes every
 * command work over the mesh (D16). `arg` is the rest of the command's line,
 * empty for one that takes none.
 */
static void commandRun(char c, const char *arg, bool remote) {
    switch (c) {
        case '?': benchIdentify(); break;
        case 'N': benchNode();     break;
        case 'r': benchReport();   break;
        case 'd':
            // No restart needed: the controller is re-armed rather than
            // reconfigured, so one run can measure the same boards with
            // correction off and on without reflashing between them.
            driftEnabled = !driftEnabled;
            driftCtl.reset(millis());
            srvCtl.reset(millis());
            DEBUG_SERIAL.printf("[BENCH] drift=%d\n", driftEnabled ? 1 : 0);
            break;
        // On a front end, the tone goes over the link to its node (D18).
        case 's': if (frontEnd) linkToneSet(true);  else benchStartSource(); break;
        case 'x': if (frontEnd) linkToneSet(false); else benchStopSource();  break;
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
            // No reboot, unlike client-only mode: nothing about the mesh id is
            // decided in setup() -- a transmitter stamps it per packet and a
            // receiver compares it per packet.
            if (arg[0] != '\0') {
                if (meshSetName(arg)) {
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
            DEBUG_SERIAL.printf("[LOSS] rx=%lu lost=%lu rec=%lu late=%lu runs=",
                                (unsigned long)rxCount, (unsigned long)seqTracker.lost,
                                (unsigned long)rxRecovered, (unsigned long)rxLate);
            rxLate = 0;
            for (int i = 0; i < 8; i++) {
                DEBUG_SERIAL.printf(i ? ",%lu" : "%lu", (unsigned long)rxLossRuns[i]);
                rxLossRuns[i] = 0;
            }
            DEBUG_SERIAL.println();
            break;
        case 'z': {
            // Explicit, like L: `z1` conceals, `z0` plays silence, bare
            // reports. Takes effect at the next lost block.
            if (arg[0] == '0' || arg[0] == '1') rxConceal = arg[0] == '1';
            DEBUG_SERIAL.printf("[RX] conceal=%d\n", rxConceal ? 1 : 0);
            break;
        }
        case 'L': {
            // Explicit on and off, not a toggle: a script that cannot
            // see the state must not be able to flip it the wrong way.
            if (arg[0] == '0' || arg[0] == '1') {
                portENTER_CRITICAL(&traceMux);
                rxTrace.reset();
                portEXIT_CRITICAL(&traceMux);
                traceOn = arg[0] == '1';
            }
            DEBUG_SERIAL.printf("[LT] trace=%d dropped=%lu\n", traceOn ? 1 : 0,
                                (unsigned long)rxTrace.dropped);
            break;
        }
        case 't': {
            const int n = atoi(arg);
            if (n >= 1 && n <= 3) txRepeat = (uint8_t)n;
            DEBUG_SERIAL.printf("[MESH] repeat=%u\n", txRepeat);
            break;
        }
        case 'D': benchSetRedundancy(false, arg); break;
        case 'X': benchSetRedundancy(true, arg);  break;
        case 'P': {
            // `P<us>`: minimum spacing between audio packets from this
            // node, until reboot; 0 = unpaced. See ESPNOW_TX_PACE_US.
            const long us = atol(arg);
            if (us >= 0 && us <= 10000) txPaceUs = (uint32_t)us;
            DEBUG_SERIAL.printf("[MESH] pace=%luus\n", (unsigned long)txPaceUs);
            break;
        }
        case 'R': {
            // `R<Mbps>`: the ESP-NOW PHY rate this node sends at, until
            // reboot. 1, 2 (DSSS) or 6..54 (OFDM). See ESPNOW_PHY_RATE.
            static const struct { int mbps; wifi_phy_rate_t r; } rates[] = {
                {1, WIFI_PHY_RATE_1M_L}, {2, WIFI_PHY_RATE_2M_L},
                {6, WIFI_PHY_RATE_6M}, {9, WIFI_PHY_RATE_9M}, {12, WIFI_PHY_RATE_12M},
                {18, WIFI_PHY_RATE_18M}, {24, WIFI_PHY_RATE_24M}, {36, WIFI_PHY_RATE_36M},
                {48, WIFI_PHY_RATE_48M}, {54, WIFI_PHY_RATE_54M}};
            const int want = atoi(arg);
            esp_err_t err = ESP_ERR_INVALID_ARG;
            for (const auto &e : rates)
                if (e.mbps == want) err = esp_wifi_config_espnow_rate(WIFI_IF_STA, e.r);
            DEBUG_SERIAL.printf("[MESH] rate=%dM -> %s\n", want, esp_err_to_name(err));
            break;
        }
        // `m1`/`m0` and `M1`/`M0` set, bare toggles. The setup page and a
        // retried question send only the first kind: a toggle sent twice,
        // because its answer was lost, undoes itself (meshCommandRetryable).
        case 'm':
            outputMuted = (arg[0] == '0' || arg[0] == '1') ? arg[0] == '1' : !outputMuted;
            DEBUG_SERIAL.printf("[OUT] mute=%d\n", outputMuted ? 1 : 0);
            break;
        case 'M':
            clientMonoOut = (arg[0] == '0' || arg[0] == '1') ? arg[0] == '1' : !clientMonoOut;
            prefs.begin(PREF_NAMESPACE, false);
            prefs.putBool(PREF_MONO_OUT, clientMonoOut);
            prefs.end();
            DEBUG_SERIAL.printf("[OUT] mono=%d\n", clientMonoOut ? 1 : 0);
            break;
        case 'v': {
            // `v<dB>`, e.g. v-6: this node's trim, kept in NVS. Bare `v`
            // reports. Out-of-range values are clamped, not refused, so
            // the reply always says what is in effect.
            if (arg[0] != '\0') {
                int db = atoi(arg);
                if (db < OUT_TRIM_MIN_DB) db = OUT_TRIM_MIN_DB;
                if (db > OUT_TRIM_MAX_DB) db = OUT_TRIM_MAX_DB;
                outTrimDb  = (int8_t)db;
                outTrimQ12 = gainQ12FromDb(db);
                prefs.begin(PREF_NAMESPACE, false);
                prefs.putChar(PREF_TRIM_DB, outTrimDb);
                prefs.end();
            }
            DEBUG_SERIAL.printf("[OUT] trim=%ddB\n", outTrimDb);
            break;
        }
        case 'p': meshStartPairing();  break;
        case 'o': meshStartOffering(); break;
        case 'B': btOpenWindow();      break;
        case 'U':
            if (arg[0] != '\0') {
                otaStartRequest(arg);
            } else if (remote && currentMode == MODE_SERVER) {
                // As the update request's own format is ignored by a server:
                // every room's stream would stop. Typed on its own port, it
                // is somebody standing at the node, and it goes.
                DEBUG_SERIAL.println("[OTA] error reason=serving");
            } else {
                otaRebootIntoUpdate();
            }
            break;
        case '@': commandAsk(arg); break;
        case '>': linkAsk(arg);    break;
        case 'C':
            // The setup network (D19): `C` opens it for CONFIG_WINDOW_MS, or
            // keeps it open that long; `C0` closes it.
            if (arg[0] == '0') configClose("C0");
            else               configOpen(CONFIG_WINDOW_MS);
            if (!cfgOpen) DEBUG_SERIAL.println("[CONFIG] closed");
            break;
        case 'I':
            // `I<name>`: this node's name, kept in NVS (D19). Bare reports.
            // At once for `@` and the page; the Bluetooth name at the next
            // start of Bluetooth, which a reboot is.
            if (arg[0] != '\0') {
                if (!meshRoomNameValid(arg)) {
                    DEBUG_SERIAL.println("[NAME] error reason=invalid (1-31 letters, digits or "
                                         "punctuation, no spaces)");
                    break;
                }
                strncpy(roomName, arg, sizeof(roomName) - 1);
                roomName[sizeof(roomName) - 1] = '\0';
                prefs.begin(PREF_NAMESPACE, false);
                prefs.putString(PREF_ROOM_NAME, roomName);
                prefs.end();
            }
            DEBUG_SERIAL.printf("[NAME] name=%s\n", roomName);
            break;
        case 'F':
            // Front-end mode (D18): like client-only mode, persisted and
            // decided in setup(), so it takes a reboot.
#ifdef ENABLE_BLUETOOTH
            prefs.begin(PREF_NAMESPACE, false);
            prefs.putBool(PREF_FRONT_END, !frontEnd);
            prefs.end();
            DEBUG_SERIAL.printf("[LINK] frontend=%d rebooting\n", frontEnd ? 0 : 1);
            DEBUG_SERIAL.flush();
            delay(50);
            ESP.restart();
#else
            DEBUG_SERIAL.println("[LINK] error reason=no_bt (a front end needs Bluetooth Classic)");
#endif
            break;
        case 'a':
            // The A2DP window: this node's own Bluetooth, a front end's as it
            // reaches the node over the link, or on a front end the phone's.
            a2dpStatsPrint();
            a2dpStatsReset();
            if (linkUp) {
                DEBUG_SERIAL.printf(
                    "[LINK] role=%s fe=%d rxB=%lu rx=%lu crc=%lu len=%lu skip=%lu gaps=%lu "
                    "badstate=%lu tx=%lu drops=%lu rate=%s\n",
                    frontEnd ? "frontend" : "node", linkFe ? 1 : 0, (unsigned long)linkRxBytes,
                    (unsigned long)linkDec->frames, (unsigned long)linkDec->crcErrors,
                    (unsigned long)linkDec->badLength, (unsigned long)linkDec->skipped,
                    (unsigned long)linkDec->seqGaps, (unsigned long)linkBadState,
                    (unsigned long)linkTxFrames, (unsigned long)linkTxDrops,
                    linkRateBad ? "refused" : "ok");
            }
            break;
        case 'f':
            a2dpForward = !a2dpForward;
            DEBUG_SERIAL.printf("[A2DP] fwd=%d\n", a2dpForward ? 1 : 0);
            break;
        case 'j':
            if (currentMode != MODE_SERVER || !outI2SActive) break;
            DEBUG_SERIAL.printf("[A2DP] jingle streaming=%d\n", txReady ? 1 : 0);
            playJingle(playConnectedSound);
            break;
#ifdef ENABLE_BLUETOOTH
        case 'w':
            // One way only: bringing ESP-NOW back after esp_wifi_stop()
            // means redoing channel and PHY rate, and a reboot does that.
            a2dpForward  = false;
            espnowActive = false;
            DEBUG_SERIAL.printf("[A2DP] wifi stop -> %s\n", esp_err_to_name(esp_wifi_stop()));
            break;
        case 'V': {
            // `V<0..127>`: the A2DP volume, as a phone's slider would set
            // it -- applied before forwarding, so it moves every room. A
            // node that dialled the PC with `k` starts at VOLUME_DEFAULT.
            if (arg[0] != '\0') a2dpSink.set_volume((uint8_t)constrain(atoi(arg), 0, 127));
            DEBUG_SERIAL.printf("[A2DP] volume=%d\n", a2dpSink.get_volume());
            break;
        }
        case 'e': {
            // Coexistence preference, at runtime, so one Bluetooth session
            // can compare them: 0 WiFi, 1 Bluetooth, 2 balance (the default).
            if (arg[0] != '\0') {
                const int p = atoi(arg);
                DEBUG_SERIAL.printf("[A2DP] coex prefer=%d -> %s\n", p,
                    esp_err_to_name(esp_coex_preference_set((esp_coex_prefer_t)p)));
            }
            break;
        }
        case 'k': {
            // `k<aa:bb:cc:dd:ee:ff>`: dial an A2DP source that is already
            // bonded, the way a headset reconnects to a phone. What lets a
            // bench reflash a server without a person clicking Connect.
            // With Bluetooth off (D17) there is nothing to dial with: `B` first.
            unsigned b[6];
            if (!btSinkStarted) {
                DEBUG_SERIAL.println("[A2DP] error reason=bt_off (B opens Bluetooth first)");
            } else if (sscanf(arg, "%x:%x:%x:%x:%x:%x",
                              &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
                esp_bd_addr_t addr;
                for (int i = 0; i < 6; i++) addr[i] = (uint8_t)b[i];
                DEBUG_SERIAL.printf("[A2DP] connect %s -> %d\n", arg,
                                    a2dpSink.connect_to(addr) ? 1 : 0);
            } else {
                DEBUG_SERIAL.println("[A2DP] error reason=address (k aa:bb:cc:dd:ee:ff)");
            }
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
#ifdef ENABLE_BLUETOOTH
    frontEnd   = prefs.getBool(PREF_FRONT_END, false);   // undone below in bench mode
#endif
    clientMonoOut = prefs.getBool(PREF_MONO_OUT, false);
    {
        const String ap = prefs.getString(PREF_AP_PASS, "");
        strlcpy(cfgApPass, ap.c_str(), sizeof(cfgApPass));
    }
    {
        // Before anything uses it: the update mode's hostname, the Bluetooth
        // name, every answer over the mesh. A stored name that no longer
        // passes (a rule tightened since) falls back to the build's.
        const String stored = prefs.getString(PREF_ROOM_NAME, "");
        if (stored.length() > 0 && meshRoomNameValid(stored.c_str())) {
            strncpy(roomName, stored.c_str(), sizeof(roomName) - 1);
            roomName[sizeof(roomName) - 1] = '\0';
        }
    }
    outTrimDb     = prefs.getChar(PREF_TRIM_DB, 0);
    if (outTrimDb < OUT_TRIM_MIN_DB || outTrimDb > OUT_TRIM_MAX_DB) outTrimDb = 0;
    outTrimQ12    = gainQ12FromDb(outTrimDb);
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
    if (frontEnd) {
        DEBUG_SERIAL.println("  Mode: FRONT END (Bluetooth for a node over the link, D18)");
    } else if (clientOnly) {
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

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ownMac, sizeof(ownMac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Update mode is a boot of its own and is decided here, before anything
    // claims the radio or the heap. Cleared before it runs, so any reset out of
    // it -- a crash included -- lands back in normal mode.
    if (otaMagic == OTA_MAGIC) {
        otaMagic = 0;
        otaRun();
    }
    {
        char info[256];
        otaDescribe(info, sizeof(info));
        DEBUG_SERIAL.printf("[OTA] boot %s\n", info);
    }

    benchMode = (benchMagic == BENCH_MAGIC);
    if (benchMode) {
        DEBUG_SERIAL.println("[BENCH] boot bench=1 (Bluetooth disabled this boot)");
        frontEnd = false;   // a bench boot is an ordinary node, front end or not
    }


    LOG_INFO("Chip: "    + String(ESP.getChipModel()));
    LOG_INFO("CPU:  "    + String(ESP.getCpuFreqMHz()) + " MHz");
    LOG_INFO("Heap: "    + String(ESP.getFreeHeap()) + " bytes");
    LOG_INFO("PSRAM: "   + String(ESP.getPsramSize()) + " bytes");

    // Startup sound, on every node: a client-only speaker is plugged in by
    // somebody who wants to hear that it is alive as much as a server's is.
    // (It used to sit under ENABLE_BLUETOOTH, from when only BT nodes had a
    // DAC.) A node with no DAC drives three idle pins for a second.
    // Skipped in bench mode: it would delay the first telemetry line and it is
    // played through an I2S driver that the client path is about to reconfigure.
    //
    // And only on a power-on (the EN button and a USB flash count as one). A
    // software restart is not somebody plugging the node in: it is update mode
    // coming and going -- twice per update, on a node that may be playing into
    // a stereo across the house (D15) -- or `c`, `n`, or the panic before a
    // rollback.
    // Nor on a front end, which has no speaker (D18).
    if (!benchMode && !frontEnd && esp_reset_reason() == ESP_RST_POWERON) {
        initI2SForTones();
        playStartupSound();
        deinitI2SForTones();
    }

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
    dcfg.targetBytes = SERVER_TARGET_BYTES;   // fed a packet at a time: runs lower
    srvCtl.begin(dcfg, millis());

    // The same controller steering on time: the error from the server's
    // schedule, in bytes, over a fixed target. See SYNC_KP.
    DriftController::Config scfg;
    scfg.targetBytes     = SYNC_FILL_BASE;
    scfg.targetCeilBytes = SYNC_FILL_BASE;   // fixed: nothing to measure
    scfg.deadbandBytes   = syncUsToBytes(SYNC_DEADBAND_US);
    scfg.kp              = SYNC_KP;
    scfg.maxRatePerSec   = SYNC_MAX_RATE;
    scfg.emaTauMs        = SYNC_EMA_TAU_MS;
    scfg.settleMs        = SYNC_SETTLE_MS;
    syncCtl.begin(scfg, millis());
    outClock.begin(CLIENT_SAMPLE_RATE);
    timeline.begin(CLIENT_SAMPLE_RATE, SYNC_MIN_SAMPLES, SYNC_MAX_AGE_MS, SYNC_JUMP_US);

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
    rxStore.init(rxStoreBuf, MESH_BLOCK_BYTES);

    if (frontEnd) {
        // Bluetooth and the link, and no WiFi at all -- which is why a WROOM,
        // with no PSRAM, can be one (D18). Bluetooth stays on for good: it is
        // this board's whole job, and its radio is not the mesh's.
        linkBegin();
#ifdef ENABLE_BLUETOOTH
        startBluetooth();
#endif
        LOG_INFO("Front end ready.");
        return;
    }

    // ESP-NOW must be up before BT to ensure coexistence layer is ready
    setupESPNow();

#ifdef ENABLE_BLUETOOTH
    if (btAllowed()) startBluetooth();
#endif
    // Every node that may take a front end listens for one (D18): wiring one is
    // all it takes.
    if (linkSourceAllowed()) linkBegin();

    // A node nobody has set up opens its setup network by itself when it is
    // plugged in (D19) -- and plays as usual meanwhile. Not one whose own chip
    // runs Bluetooth, which would have to pause it: a phone could not find a
    // fresh WROVER for five minutes. The long hold opens it there.
    if (!benchMode && !ownBt() && cfgApPass[0] == '\0' && esp_reset_reason() == ESP_RST_POWERON) {
        configOpen(CONFIG_FIRST_WINDOW_MS);
    }

    LOG_INFO("Setup complete. Entering DISCOVERY...");
    if (benchMode) benchIdentify();
}

// ============================================================================
// Main Loop
// ============================================================================

void loop() {
    if (frontEnd) {
        // Bluetooth and the link, nothing else (D18): no mesh, no button, no
        // speaker. Commands from its own port and from its node.
        benchServiceSerial();
        linkServiceFrontEnd();
        otaServiceConfirm();
        vTaskDelay(1);
        return;
    }

    // Always listening for host commands, in every mode — this is how a node is
    // put into bench mode in the first place.
    benchServiceSerial();
    traceService();
    benchServiceSource();
    meshServiceButton();
    meshServicePairing();
    meshServiceOffer();
    meshServiceControl();
    otaServiceConfirm();
    btServiceWindow();
    linkServiceNode();
    configService();

    switch (currentMode) {

        case MODE_DISCOVERY:
            if (btConnected) {   // this chip's phone, or a front end's (D18)
                enterServer();
                break;
            }
            // Not while a short press holds Bluetooth open for a phone (D17):
            // joining would switch it straight off again.
            if (rxActive && espnowActive && !btWindow &&
                (long)(millis() - clientRetryAfterMs) >= 0) {
                enterClient();
            }
            break;

        case MODE_SERVER:
            // A bench source's SERVER is its own tone, with no phone to lose,
            // so none of this applies -- without the guard it would leave
            // SERVER on the first pass.
            if (!benchSource) {
                if (doConnectSound) {
                    doConnectSound = false;
                    playJingle(playConnectedSound);
                }
                if (!btConnected) {
                    if (doDisconnectSound) {
                        doDisconnectSound = false;
                        playJingle(playDisconnectedSound);
                    }
                    enterDiscovery();
                    break;
                }
            }
            driveRingI2S();   // a Bluetooth server's own speaker; nothing on a bench source
            break;

        case MODE_CLIENT:
            driveRingI2S();

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
                     " fgn=" + String(rxForeign) +
                     " sync=" + String(outSynced ? 1 : 0) +
                     " se=" + String(syncErrUs) + "us" +
                     " sjmp=" + String(syncJumps));
        } else if (currentMode == MODE_SERVER) {
            LOG_INFO(String("Status: mode=") + modeStr +
                     " heap=" + String(ESP.getFreeHeap()) +
                     " maxalloc=" + String(ESP.getMaxAllocHeap()) +
                     " tx=" + String(txSent) +
                     " jitter=" + String(jbuf.fill()) + "B" +
                     " und=" + String(rxUnderrun) +
                     " dry=" + String(outDry) +
                     " tgt=" + String(srvCtl.target()) +
                     " ins=" + String(srvCtl.inserted) +
                     " drp=" + String(srvCtl.dropped) +
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
