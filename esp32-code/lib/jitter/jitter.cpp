#include "jitter.h"

#include <string.h>

static inline int minInt(int a, int b) { return a < b ? a : b; }

bool JitterBuffer::init(uint8_t *storage, int size) {
    if (storage == nullptr) return false;
    if (size <= 0) return false;
    if ((size & (size - 1)) != 0) return false;   // must be a power of two

    buf_  = storage;
    size_ = size;
    mask_ = size - 1;
    wr_   = 0;
    rd_   = 0;
    return true;
}

void JitterBuffer::reset() {
    wr_ = 0;
    rd_ = 0;
}

int JitterBuffer::fill() const {
    int f = wr_ - rd_;
    return f < 0 ? f + size_ : f;
}

int JitterBuffer::free() const {
    return size_ - 1 - fill();
}

bool JitterBuffer::pushBlock(const uint8_t *data, int len) {
    if (len <= 0) return true;
    if (buf_ == nullptr) return false;
    if (free() < len) return false;

    const int w     = wr_;
    const int first = minInt(len, size_ - w);
    memcpy(buf_ + w, data, first);
    if (len > first) memcpy(buf_, data + first, len - first);
    wr_ = (w + len) & mask_;
    return true;
}

bool JitterBuffer::pushSilence(int len) {
    if (len <= 0) return true;
    if (buf_ == nullptr) return false;
    if (free() < len) return false;

    const int w     = wr_;
    const int first = minInt(len, size_ - w);
    memset(buf_ + w, 0, first);
    if (len > first) memset(buf_, 0, len - first);
    wr_ = (w + len) & mask_;
    return true;
}

bool JitterBuffer::peek(uint8_t *out, int len) const {
    if (len <= 0) return true;
    if (buf_ == nullptr) return false;
    if (fill() < len) return false;

    const int r     = rd_;
    const int first = minInt(len, size_ - r);
    memcpy(out, buf_ + r, first);
    if (len > first) memcpy(out + first, buf_, len - first);
    return true;
}

void JitterBuffer::advance(int len) {
    if (len <= 0) return;
    rd_ = (rd_ + len) & mask_;
}
