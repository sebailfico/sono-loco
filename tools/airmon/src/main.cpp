// SonoLoco air monitor.
//
// A spare ESP32 in WiFi promiscuous mode, parked on the mesh channel, saying
// once a second what is actually on the air: how busy the channel is, how many
// of the frames are SonoLoco's own ESP-NOW packets and how loud they arrive,
// who else is transmitting (beacons by SSID, data by transmitter), and how many
// frames failed their checksum -- the nearest thing a WiFi radio has to a
// collision counter.
//
// It exists because the mesh nodes can only count what they missed. On
// 2026-09-14 four clients lost packets in bursts at the same instants while the
// source's counters were clean, and nothing on any node could say what was
// winning those collisions. This board can.
//
// Serial, 115200:
//   c<n>   park on channel n (1-13) and monitor it   (default: 1)
//   s      survey: one second on each channel 1-13, then back to monitoring
//   ?      print the current channel
//
// Output, one line a second:
//   [AIR] t=<ms> ch=<n> air=<%> frames=<n> bad=<n> espnow=<n> en_rssi=<avg>/<min>
//         beacons=<n> bss=<n> data=<n> databytes=<n> mgmt=<n> ctl=<n> top=<mac>:<n>,...
// and, on each new BSSID heard: [BSS] <mac> ch=<n> rssi=<dBm> ssid=<name>
//
// Airtime is estimated from length and rate: DSSS 192 us long / 96 us short
// preamble, OFDM 20 us, HT about 40 us, plus bytes*8/rate. It is a per-second
// duty cycle of everything this radio decoded plus everything it started to
// decode and lost, and it is only ever an underestimate -- a frame too damaged
// to see is not counted at all.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <string.h>

static const uint8_t ESPNOW_OUI[3] = {0x18, 0xFE, 0x34};

// ---------------------------------------------------------------------------
// Counters written by the promiscuous callback (WiFi task), read and reset by
// loop() once a second. Plain volatile ints; a torn read costs one line.
// ---------------------------------------------------------------------------
struct Talker { uint8_t mac[6]; uint32_t n; };
static const int TOP_N = 12;

static volatile uint32_t cFrames, cBad, cEspnow, cBeacon, cData, cDataBytes, cMgmt, cCtl;
static volatile uint32_t cAirUs;
static volatile int32_t  cEnRssiSum, cEnRssiMin, cEnRssiN;
static Talker top[TOP_N];
static volatile int topN;
static volatile uint8_t curChannel = 1;

// Beacon table, kept across seconds so an SSID is announced once.
struct Bss { uint8_t mac[6]; uint8_t ch; int8_t rssi; char ssid[33]; bool announced; };
static const int BSS_N = 48;
static Bss bss[BSS_N];
static volatile int bssN;
static volatile bool bssDirty;

static uint32_t rateKbps(const wifi_pkt_rx_ctrl_t &rc, uint32_t &preambleUs) {
    if (rc.sig_mode == 0) {                 // 11b / 11g
        switch (rc.rate) {
            case 0x00: preambleUs = 192; return 1000;
            case 0x01: preambleUs = 192; return 2000;
            case 0x02: preambleUs = 192; return 5500;
            case 0x03: preambleUs = 192; return 11000;
            case 0x05: preambleUs = 96;  return 2000;
            case 0x06: preambleUs = 96;  return 5500;
            case 0x07: preambleUs = 96;  return 11000;
            case 0x0B: preambleUs = 20;  return 6000;
            case 0x0F: preambleUs = 20;  return 9000;
            case 0x0A: preambleUs = 20;  return 12000;
            case 0x0E: preambleUs = 20;  return 18000;
            case 0x09: preambleUs = 20;  return 24000;
            case 0x0D: preambleUs = 20;  return 36000;
            case 0x08: preambleUs = 20;  return 48000;
            case 0x0C: preambleUs = 20;  return 54000;
            default:   preambleUs = 192; return 1000;
        }
    }
    // HT: MCS0-7 at 20 MHz, long GI: 6.5 13 19.5 26 39 52 58.5 65 Mbps
    static const uint16_t ht20[8] = {6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000};
    preambleUs = 40;
    uint32_t r = ht20[rc.mcs & 7];
    if (rc.cwb) r *= 2;
    if (rc.sgi) r = r * 10 / 9;
    return r;
}

static void noteTalker(const uint8_t *mac) {
    int n = topN;
    for (int i = 0; i < n; i++) {
        if (memcmp(top[i].mac, mac, 6) == 0) { top[i].n++; return; }
    }
    if (n < TOP_N) { memcpy(top[n].mac, mac, 6); top[n].n = 1; topN = n + 1; }
}

static void noteBss(const uint8_t *mac, const uint8_t *body, int bodyLen, int8_t rssi) {
    int n = bssN;
    for (int i = 0; i < n; i++) {
        if (memcmp(bss[i].mac, mac, 6) == 0) { bss[i].rssi = rssi; return; }
    }
    if (n >= BSS_N) return;
    Bss &b = bss[n];
    memcpy(b.mac, mac, 6);
    b.rssi = rssi; b.ch = 0; b.ssid[0] = 0; b.announced = false;
    // Fixed fields: timestamp 8, interval 2, capabilities 2. Then IEs.
    int i = 12;
    while (i + 2 <= bodyLen) {
        uint8_t id = body[i], len = body[i + 1];
        if (i + 2 + len > bodyLen) break;
        if (id == 0 && len <= 32) { memcpy(b.ssid, body + i + 2, len); b.ssid[len] = 0; }
        if (id == 3 && len == 1)  { b.ch = body[i + 2]; }
        i += 2 + len;
    }
    if (b.ssid[0] == 0) strcpy(b.ssid, "<hidden>");
    bssN = n + 1;
    bssDirty = true;
}

static void IRAM_ATTR onFrame(void *buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const wifi_pkt_rx_ctrl_t &rc = p->rx_ctrl;

    uint32_t pre;
    uint32_t kbps = rateKbps(rc, pre);
    cAirUs += pre + ((uint32_t)rc.sig_len * 8000u) / kbps;
    cFrames++;

    if (rc.rx_state != 0) { cBad++; return; }       // FCS or other RX error: header untrustworthy

    const uint8_t *h = p->payload;
    uint8_t fc0 = h[0];
    uint8_t ftype = (fc0 >> 2) & 3, subtype = (fc0 >> 4) & 0xF;
    const uint8_t *addr2 = h + 10;

    if (ftype == 0) {                                // management
        if (subtype == 0x0D && rc.sig_len > 28 &&
            h[24] == 127 && memcmp(h + 25, ESPNOW_OUI, 3) == 0) {
            cEspnow++;
            cEnRssiSum += rc.rssi; cEnRssiN++;
            if (rc.rssi < cEnRssiMin) cEnRssiMin = rc.rssi;
            noteTalker(addr2);
        } else if (subtype == 0x08) {
            cBeacon++;
            noteBss(h + 16, h + 24, (int)rc.sig_len - 24 - 4, rc.rssi);
        } else {
            cMgmt++;
        }
    } else if (ftype == 1) {
        cCtl++;
    } else if (ftype == 2) {
        cData++;
        cDataBytes += rc.sig_len;
        noteTalker(addr2);
    }
}

static void setChannel(uint8_t ch) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    curChannel = ch;
}

static void resetCounters() {
    cFrames = cBad = cEspnow = cBeacon = cData = cDataBytes = cMgmt = cCtl = 0;
    cAirUs = 0;
    cEnRssiSum = 0; cEnRssiN = 0; cEnRssiMin = 0;
    topN = 0;
}

static String macStr(const uint8_t *m) {
    char s[18];
    snprintf(s, sizeof s, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(s);
}

static void report(uint32_t windowMs) {
    // Snapshot then reset, so the callback's next second starts clean.
    uint32_t frames = cFrames, bad = cBad, en = cEspnow, bc = cBeacon, dt = cData, db = cDataBytes,
             mg = cMgmt, ct = cCtl, air = cAirUs;
    int32_t rs = cEnRssiSum, rn = cEnRssiN, rmin = cEnRssiMin;
    int tn = topN;
    Talker t[TOP_N];
    memcpy(t, top, sizeof t);
    resetCounters();

    // Top talkers, sorted, at most 4.
    for (int i = 0; i < tn; i++)
        for (int j = i + 1; j < tn; j++)
            if (t[j].n > t[i].n) { Talker x = t[i]; t[i] = t[j]; t[j] = x; }

    String line = "[AIR] t=" + String(millis()) + " ch=" + String(curChannel) +
                  " air=" + String(100.0f * air / (windowMs * 1000.0f), 1) + "%" +
                  " frames=" + String(frames) + " bad=" + String(bad) +
                  " espnow=" + String(en) +
                  " en_rssi=" + (rn ? String(rs / rn) + "/" + String(rmin) : String("-/-")) +
                  " beacons=" + String(bc) + " bss=" + String(bssN) +
                  " data=" + String(dt) + " databytes=" + String(db) +
                  " mgmt=" + String(mg) + " ctl=" + String(ct) + " top=";
    for (int i = 0; i < tn && i < 4; i++) {
        if (i) line += ",";
        line += macStr(t[i].mac) + ":" + String(t[i].n);
    }
    Serial.println(line);

    if (bssDirty) {
        bssDirty = false;
        for (int i = 0; i < bssN; i++) {
            if (bss[i].announced) continue;
            bss[i].announced = true;
            Serial.printf("[BSS] %s ch=%u rssi=%d ssid=%s\n",
                          macStr(bss[i].mac).c_str(), bss[i].ch, bss[i].rssi, bss[i].ssid);
        }
    }
}

static void survey() {
    uint8_t back = curChannel;
    Serial.println("[SURVEY] one second per channel");
    Serial.println("[SURVEY] ch   air%  frames   bad  beacons  bss(on ch)  data");
    for (uint8_t ch = 1; ch <= 13; ch++) {
        setChannel(ch);
        delay(50);
        resetCounters();
        delay(1000);
        uint32_t frames = cFrames, bad = cBad, bc = cBeacon, dt = cData, air = cAirUs;
        int onCh = 0;
        for (int i = 0; i < bssN; i++) if (bss[i].ch == ch) onCh++;
        Serial.printf("[SURVEY] %2u  %5.1f  %6lu  %4lu  %7lu  %10d  %5lu\n",
                      ch, 100.0f * air / 1e6f, (unsigned long)frames, (unsigned long)bad,
                      (unsigned long)bc, onCh, (unsigned long)dt);
    }
    for (int i = 0; i < bssN; i++) {
        Serial.printf("[BSS] %s ch=%u rssi=%d ssid=%s\n",
                      macStr(bss[i].mac).c_str(), bss[i].ch, bss[i].rssi, bss[i].ssid);
        bss[i].announced = true;
    }
    bssDirty = false;
    setChannel(back);
    resetCounters();
    Serial.println("[SURVEY] done");
}

void setup() {
    Serial.begin(115200);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL | WIFI_PROMIS_FILTER_MASK_FCSFAIL
    };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(onFrame);
    esp_wifi_set_promiscuous(true);
    setChannel(1);
    resetCounters();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    Serial.println();
    Serial.println("[AIRMON] SonoLoco air monitor, mac " + macStr(mac) + ", channel 1. c<n> s ?");
}

void loop() {
    static uint32_t last = millis();

    while (Serial.available()) {
        int c = Serial.read();
        if (c == 'c') {
            delay(20);
            int n = Serial.parseInt();
            if (n >= 1 && n <= 13) { setChannel(n); resetCounters(); last = millis();
                                     Serial.printf("[AIRMON] channel %d\n", n); }
        } else if (c == 's') {
            survey();
            last = millis();
        } else if (c == '?') {
            Serial.printf("[AIRMON] channel %u\n", curChannel);
        }
    }

    uint32_t now = millis();
    if (now - last >= 1000) {
        report(now - last);
        last = now;
    }
}
