#include "seqtracker.h"

SeqTracker::SeqTracker(uint16_t resyncThreshold, int maxGapFillPkts)
    : resyncThreshold_(resyncThreshold), maxGapFillPkts_(maxGapFillPkts) {}

void SeqTracker::reset() {
    lastSeq_ = 0;
    first_   = true;
    lost     = 0;
    dupe     = 0;
    resync   = 0;
}

SeqResult SeqTracker::update(uint16_t seq) {
    SeqResult r = {true, 0};

    if (first_) {
        first_ = false;
    } else if (seq == lastSeq_) {
        dupe++;
        r.accept = false;
        return r;   // playing an exact retransmit twice is an audible stutter
    } else {
        // Unsigned on purpose: this is modular arithmetic, and it wraps
        // correctly across 65535 -> 0 for the normal in-order case.
        const uint16_t gap = (uint16_t)(seq - lastSeq_ - 1);

        if (gap >= resyncThreshold_) {
            // Anything *behind* lastSeq_ lands here as ~65535. Re-baseline on
            // the new sequence and play the payload instead of accounting it
            // as tens of thousands of lost packets.
            resync++;
        } else if (gap > 0) {
            lost += gap;
            r.fillPackets = (int)gap < maxGapFillPkts_ ? (int)gap : maxGapFillPkts_;
        }
    }

    lastSeq_ = seq;
    return r;
}
