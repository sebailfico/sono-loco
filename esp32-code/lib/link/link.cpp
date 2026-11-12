#include "link.h"

#include <string.h>

uint16_t linkCrc16(const uint8_t *data, size_t len, uint16_t crc) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t linkEncode(uint8_t *out, size_t outSize, uint8_t type, uint8_t seq,
                  const uint8_t *payload, size_t len) {
    const size_t total = LINK_HEADER_BYTES + len + LINK_CRC_BYTES;
    if (len > LINK_MAX_PAYLOAD || total > outSize) return 0;
    out[0] = LINK_SYNC0;
    out[1] = LINK_SYNC1;
    out[2] = type;
    out[3] = seq;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)(len >> 8);
    if (len > 0) memcpy(out + LINK_HEADER_BYTES, payload, len);
    const uint16_t crc = linkCrc16(out + 2, LINK_HEADER_BYTES - 2 + len);
    out[LINK_HEADER_BYTES + len]     = (uint8_t)(crc & 0xFF);
    out[LINK_HEADER_BYTES + len + 1] = (uint8_t)(crc >> 8);
    return total;
}

void LinkDecoder::reset() {
    have_    = 0;
    consume_ = 0;
}

void LinkDecoder::drop(size_t n) {
    if (n >= have_) {
        have_ = 0;
        return;
    }
    memmove(buf_, buf_ + n, have_ - n);
    have_ -= n;
}

bool LinkDecoder::scan(LinkFrame *frame) {
    while (have_ > 0) {
        if (buf_[0] != LINK_SYNC0 || (have_ >= 2 && buf_[1] != LINK_SYNC1)) {
            skipped++;
            drop(1);
            continue;
        }
        if (have_ < LINK_HEADER_BYTES) return false;
        const size_t len = (size_t)buf_[4] | ((size_t)buf_[5] << 8);
        if (len > LINK_MAX_PAYLOAD) {
            // A sync and a nonsense length: noise that happened to start with
            // the sync bytes. Step past the first of them and look again.
            badLength++;
            drop(1);
            continue;
        }
        const size_t total = LINK_HEADER_BYTES + len + LINK_CRC_BYTES;
        if (have_ < total) return false;
        const uint16_t want = (uint16_t)(buf_[LINK_HEADER_BYTES + len] |
                                         (buf_[LINK_HEADER_BYTES + len + 1] << 8));
        if (linkCrc16(buf_ + 2, LINK_HEADER_BYTES - 2 + len) != want) {
            // Not a frame after all -- or a frame with a byte gone. The real
            // next sync may be anywhere in what was buffered on its account,
            // so step one byte, not the whole claimed length.
            crcErrors++;
            drop(1);
            continue;
        }
        frame->type    = buf_[2];
        frame->seq     = buf_[3];
        frame->len     = (uint16_t)len;
        frame->payload = buf_ + LINK_HEADER_BYTES;
        if (seqKnown_) seqGaps += (uint8_t)(frame->seq - lastSeq_ - 1);
        seqKnown_ = true;
        lastSeq_  = frame->seq;
        frames++;
        consume_ = total;
        return true;
    }
    return false;
}

size_t LinkDecoder::push(const uint8_t *data, size_t len, LinkFrame *frame, bool *got) {
    *got = false;
    if (consume_ > 0) {
        drop(consume_);
        consume_ = 0;
        // What followed the last frame in the buffer -- left there by a resync
        // -- may already hold the next one.
        if (scan(frame)) {
            *got = true;
            return 0;
        }
    }
    size_t taken = 0;
    while (taken < len) {
        // Sync hunting a byte at a time; inside a frame, as much as it needs.
        size_t want = 1;
        if (have_ >= LINK_HEADER_BYTES) {
            const size_t flen  = (size_t)buf_[4] | ((size_t)buf_[5] << 8);
            const size_t total = LINK_HEADER_BYTES + flen + LINK_CRC_BYTES;
            if (flen <= LINK_MAX_PAYLOAD && total > have_) want = total - have_;
        }
        if (want > len - taken) want = len - taken;
        memcpy(buf_ + have_, data + taken, want);
        have_ += want;
        taken += want;
        if (scan(frame)) {
            *got = true;
            return taken;
        }
    }
    return taken;
}

size_t linkPackState(const LinkState &s, uint8_t *out, size_t outSize) {
    if (outSize < LINK_STATE_BYTES) return 0;
    out[0] = LINK_PROTOCOL;
    out[1] = (uint8_t)((s.connected ? 1 : 0) | (s.streaming ? 2 : 0));
    out[2] = (uint8_t)(s.sampleRate & 0xFF);
    out[3] = (uint8_t)(s.sampleRate >> 8);
    out[4] = s.volume;
    return LINK_STATE_BYTES;
}

bool linkUnpackState(const uint8_t *payload, size_t len, LinkState *s) {
    if (len < LINK_STATE_BYTES || payload[0] != LINK_PROTOCOL) return false;
    s->connected  = (payload[1] & 1) != 0;
    s->streaming  = (payload[1] & 2) != 0;
    s->sampleRate = (uint16_t)(payload[2] | (payload[3] << 8));
    s->volume     = payload[4];
    return true;
}
