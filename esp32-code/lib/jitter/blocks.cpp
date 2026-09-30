#include "blocks.h"

#include <string.h>

bool BlockStore::init(uint8_t *storage, int blockBytes) {
    if (storage == nullptr || blockBytes <= 0) return false;
    buf_   = storage;
    bytes_ = blockBytes;
    clear();
    return true;
}

void BlockStore::clear() {
    for (auto &u : used_) u = false;
}

void BlockStore::put(uint16_t seq, const uint8_t *block) {
    if (buf_ == nullptr) return;
    const int i = seq % SLOTS;
    memcpy(buf_ + i * bytes_, block, bytes_);
    seq_[i]  = seq;
    used_[i] = true;
}

const uint8_t *BlockStore::get(uint16_t seq) const {
    const int i = seq % SLOTS;
    if (buf_ == nullptr || !used_[i] || seq_[i] != seq) return nullptr;
    return buf_ + i * bytes_;
}

void xorBlocks(uint8_t *out, const uint8_t *a, const uint8_t *b, int len) {
    for (int i = 0; i < len; i++) out[i] = a[i] ^ b[i];
}

bool parityRebuildable(const BlockStore &store, uint16_t seq, int far,
                       uint16_t *missing, const uint8_t **known) {
    if (far < 2) return false;   // n-1 and n-1: the parity is all zeroes
    const uint16_t near  = (uint16_t)(seq - 1);
    const uint16_t older = (uint16_t)(seq - far);
    const uint8_t *hn = store.get(near);
    const uint8_t *ho = store.get(older);
    if (hn == nullptr && ho != nullptr) {
        *missing = near;
        *known   = ho;
        return true;
    }
    if (hn != nullptr && ho == nullptr) {
        *missing = older;
        *known   = hn;
        return true;
    }
    return false;
}
