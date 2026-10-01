#include "losstrace.h"

#include <string.h>

void LossTrace::reset() {
    started_ = false;
    pending_ = 0;
}

void LossTrace::add(uint16_t seq, uint32_t lostBefore) {
    // A run longer than the ring cannot be held; it can only be a break.
    if (lostBefore >= (uint32_t)BITS) {
        started_   = false;
        lostBefore = 0;
    }
    const uint16_t first = (uint16_t)(seq - lostBefore);
    const int      n     = (int)lostBefore + 1;

    const bool follows = started_ && (uint16_t)(tailSeq_ + pending_) == first;
    if (!follows || pending_ + n > BITS) {
        dropped += (uint32_t)pending_;
        tailSeq_ = first;
        pending_ = 0;
        started_ = true;
    }

    for (int k = 0; k < n; k++) {
        const uint32_t i = (tailIdx_ + (uint32_t)pending_ + (uint32_t)k) % BITS;
        const uint8_t  m = (uint8_t)(1u << (i % 8));
        if (k < n - 1) bits_[i / 8] |= m;          // missed
        else           bits_[i / 8] &= (uint8_t)~m;  // this one arrived
    }
    pending_ += n;
}

int LossTrace::take(uint16_t *start, uint8_t *out, int maxCount) {
    const int count = pending_ < maxCount ? pending_ : maxCount;
    if (count <= 0) return 0;
    memset(out, 0, (size_t)(count + 7) / 8);
    for (int k = 0; k < count; k++) {
        const uint32_t i = (tailIdx_ + (uint32_t)k) % BITS;
        if (bits_[i / 8] & (1u << (i % 8))) out[k / 8] |= (uint8_t)(1u << (k % 8));
    }
    *start   = tailSeq_;
    tailIdx_ = (tailIdx_ + (uint32_t)count) % BITS;
    tailSeq_ = (uint16_t)(tailSeq_ + count);
    pending_ -= count;
    return count;
}
