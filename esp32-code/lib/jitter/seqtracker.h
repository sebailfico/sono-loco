#ifndef SEQTRACKER_H
#define SEQTRACKER_H

/**
 * Packet sequence accounting for the CLIENT receive path.
 *
 * Split out of the ESP-NOW recv callback for the same reason as JitterBuffer:
 * it is pure arithmetic, it has already been wrong once in a way that was very
 * hard to see on a board, and it can be tested on the host.
 *
 * The bug worth remembering: `seq` is uint16_t, so a duplicate or reordered
 * frame — or a server that rebooted and restarted its counter at 0 — computes
 * a "gap" of ~65535. Charging that to the lost counter and splicing in the
 * corresponding silence turned one stray packet into a stream of noise.
 */

#include <stdint.h>

struct SeqResult {
    /** false = drop this packet entirely (an exact retransmit). */
    bool accept;
    /** Whole packets of silence to splice in before it, to preserve timing. */
    int fillPackets;
};

class SeqTracker {
public:
    /**
     * @param resyncThreshold  a computed gap at or above this is treated as a
     *                         stream restart rather than as loss
     * @param maxGapFillPkts   cap on silence packets injected for one gap, so a
     *                         long outage cannot flood the jitter buffer
     */
    SeqTracker(uint16_t resyncThreshold, int maxGapFillPkts);

    /** Feed the sequence number of a freshly received packet. */
    SeqResult update(uint16_t seq);

    /** Forget the stream. Next packet is treated as the first one. */
    void reset();

    // Counters. Written from the recv callback, read from loop(), hence
    // volatile — never printed from the callback itself.
    volatile uint32_t lost   = 0;   // packets missing per sequence numbers
    volatile uint32_t dupe   = 0;   // exact retransmits, discarded
    volatile uint32_t resync = 0;   // sequence jumped — stream restarted

private:
    uint16_t resyncThreshold_;
    int      maxGapFillPkts_;
    uint16_t lastSeq_ = 0;
    bool     first_   = true;
};

#endif  // SEQTRACKER_H
