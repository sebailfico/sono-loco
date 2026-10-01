#ifndef LOSSTRACE_H
#define LOSSTRACE_H

/**
 * Which packets a client heard and which it missed, one bit each, in order.
 *
 * The loss counters say how much was lost; they cannot say what a different
 * redundancy would have saved, because that depends on *which* packets went
 * missing -- a lone loss next to a run, two losses 11 apart. And comparing
 * schemes live means alternating minutes whose conditions swung 3× (D13).
 * With the outcome of every packet recorded, one recording scores every
 * scheme on the same losses (tools/btlisten/losstrace.py).
 *
 * The receive path add()s each accepted packet with the run of losses the
 * sequence tracker charged before it; loop() take()s what has accumulated and
 * prints it. Pure logic, tested on the host with the rest of lib/jitter; the
 * caller holds whatever lock its two tasks need.
 *
 * Outcomes are kept in one unbroken numbering. Anything that breaks it -- a
 * resync, a gap the caller says nothing about, a reader that fell BITS
 * behind -- discards what was not yet taken and starts again at the packet
 * that broke it, counted in `dropped`. A reader sees the break as a line
 * whose first seq does not follow the last.
 */

#include <stdint.h>

class LossTrace {
public:
    /** Outcomes held between takes: ~10 s of a 387 pkt/s stream. */
    static const int BITS = 4096;

    /** Forget everything; the next add() starts a new numbering. */
    void reset();

    /**
     * Packet `seq` arrived, and the `lostBefore` packets just before it did
     * not. A packet that does not follow the last one -- a resync -- starts
     * a new numbering at seq - lostBefore.
     */
    void add(uint16_t seq, uint32_t lostBefore);

    /**
     * Up to `maxCount` outcomes, oldest first: bit i of out[i / 8] (LSB
     * first) is 1 if packet *start + i was lost. Returns how many (0 if none
     * are waiting); `out` must hold (maxCount + 7) / 8 bytes.
     */
    int take(uint16_t *start, uint8_t *out, int maxCount);

    /** Outcomes waiting to be taken. */
    int pending() const { return pending_; }

    /** Outcomes discarded at a break, never taken. */
    uint32_t dropped = 0;

private:
    uint8_t  bits_[BITS / 8] = {};
    uint32_t tailIdx_ = 0;       // ring index of the oldest outcome not taken
    uint16_t tailSeq_ = 0;       // and its packet's seq
    int      pending_ = 0;
    bool     started_ = false;
};

#endif  // LOSSTRACE_H
