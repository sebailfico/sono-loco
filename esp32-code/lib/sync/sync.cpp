#include "sync.h"

int32_t syncFramesToUs(int32_t frames, uint32_t rate) {
    const int64_t num = (int64_t)frames * 1000000LL;
    const int64_t half = (int64_t)(rate / 2);
    return (int32_t)(num >= 0 ? (num + half) / (int64_t)rate : (num - half) / (int64_t)rate);
}

int32_t syncUsToFrames(int32_t us, uint32_t rate) {
    const int64_t num = (int64_t)us * (int64_t)rate;
    return (int32_t)(num >= 0 ? (num + 500000LL) / 1000000LL : (num - 500000LL) / 1000000LL);
}

uint32_t syncDmaAheadFrames(uint32_t framesWritten, uint32_t bufCount, uint32_t bufLen) {
    // The driver fills its buffers in order from the moment it is installed,
    // so the write position inside the current buffer is the running count
    // modulo the buffer length -- and a write that ended exactly on a boundary
    // has filled that buffer completely, not started the next one.
    uint32_t inBuf = framesWritten % bufLen;
    if (inBuf == 0) inBuf = bufLen;
    return (bufCount - 1) * bufLen + inBuf;
}

void PlayoutClock::observe(uint32_t nowUs, uint32_t nextFrame, uint32_t aheadFrames) {
    const uint32_t measured = nowUs + (uint32_t)syncFramesToUs((int32_t)aheadFrames, rate_);
    observations++;
    if (!valid_) {
        anchorFrame_ = nextFrame;
        anchorUs_    = measured;
        valid_       = true;
        return;
    }
    const uint32_t predicted = playUs(nextFrame);
    const int32_t  late      = (int32_t)(measured - predicted);
    anchorFrame_ = nextFrame;
    anchorUs_    = late < 0 ? measured : predicted + (uint32_t)(late / 8);
}

uint32_t PlayoutClock::playUs(uint32_t frame) const {
    return anchorUs_ + (uint32_t)syncFramesToUs((int32_t)(frame - anchorFrame_), rate_);
}

void ServerTimeline::begin(uint32_t rate, uint32_t minSamples, uint32_t maxAgeMs,
                           int32_t shiftUs) {
    rate_       = rate;
    minSamples_ = minSamples ? minSamples : 1;
    maxAgeMs_   = maxAgeMs;
    shiftUs_    = shiftUs;
    clear();
}

void ServerTimeline::clear() {
    count_ = 0;
    for (auto &p : hist_) p.used = false;
    next_  = 0;
    later_ = 0;
    last_  = -1;
}

void ServerTimeline::add(uint32_t frame, uint32_t dueUs) {
    if (count_ == 0) {
        refFrame_ = frame;
        refUs_    = dueUs;
        minDelta_ = 0;
        count_    = 1;
        return;
    }
    // Where this point sits against the window's first, once the frames
    // between them are accounted for: negative means earlier on the line.
    const int32_t d = (int32_t)(dueUs - refUs_) -
                      syncFramesToUs((int32_t)(frame - refFrame_), rate_);
    if (d < minDelta_) minDelta_ = d;
    count_++;
}

void ServerTimeline::harvest(uint32_t nowMs) {
    if (count_ >= minSamples_) {
        const uint32_t frame = refFrame_;
        const uint32_t us    = refUs_ + (uint32_t)minDelta_;
        if (valid(nowMs)) {
            if ((int32_t)(us - dueUs(frame, nowMs)) > shiftUs_) {
                // Late by more than a slow window explains. The second in a
                // row is a new schedule: keep it and the one before, drop the
                // rest.
                if (++later_ >= 2) {
                    for (int i = 0; i < HISTORY; i++)
                        if (i != last_) hist_[i].used = false;
                    later_ = 0;
                }
            } else {
                later_ = 0;
            }
        }
        Point &p = hist_[next_];
        p.frame = frame;
        p.us    = us;
        p.atMs  = nowMs;
        p.used  = true;
        last_   = next_;
        next_   = (next_ + 1) % HISTORY;
    }
    count_ = 0;
}

bool ServerTimeline::valid(uint32_t nowMs) const {
    for (const auto &p : hist_)
        if (p.used && (int32_t)(nowMs - p.atMs) <= (int32_t)maxAgeMs_) return true;
    return false;
}

uint32_t ServerTimeline::dueUs(uint32_t frame, uint32_t nowMs) const {
    bool     have = false;
    uint32_t best = 0;
    for (const auto &p : hist_) {
        if (!p.used || (int32_t)(nowMs - p.atMs) > (int32_t)maxAgeMs_) continue;
        const uint32_t t = p.us + (uint32_t)syncFramesToUs((int32_t)(frame - p.frame), rate_);
        if (!have || (int32_t)(t - best) < 0) best = t;
        have = true;
    }
    return best;
}
