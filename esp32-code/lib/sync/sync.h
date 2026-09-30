#ifndef SYNC_H
#define SYNC_H

/**
 * When a node's speaker plays a given frame, and when the server's does.
 *
 * Every node that plays the stream -- the clients and, since 2026-09-30, the
 * Bluetooth server itself -- feeds I2S from the same ring. Measured with a
 * microphone before this existed, a client played 44-51 ms after the server:
 * the server went through the A2DP library's 46 ms I2S ring, the clients
 * through a 91 ms prefill that armed wherever in a packet burst it happened to
 * fill. An echo between rooms, and a different one on every stream start.
 *
 * The fix is a shared idea of time rather than a matched pair of delays:
 *
 *   - Each node knows when its own output plays a frame (PlayoutClock). A
 *     blocking i2s_write() returns just after a DMA buffer finished playing, so
 *     at that moment the frames queued ahead of the next write are a known
 *     number of whole buffers plus the part of the current one already
 *     written -- exact to the interrupt latency, with no driver internals read.
 *   - The server stamps every packet with how long after its sending the
 *     block's first frame plays on the server's speaker. A client adds that to
 *     the time it received the packet and gets the server's play time on its
 *     own clock -- late by however long the frame took over the air, which is
 *     why ServerTimeline keeps the *earliest* such estimate: the packets that
 *     crossed fastest carry the truth, and every slower one can only be later.
 *   - The client steers its own output onto that line.
 *
 * No clock synchronisation protocol, no shared timebase: a relative stamp and
 * a minimum. Pure logic, no Arduino or ESP-IDF, tested on the host (or a
 * board) in test/test_sync.
 *
 * Times are microseconds on the node's own 32-bit clock (micros()), and wrap
 * every 71 minutes; frames are 32-bit counters that wrap every 27 hours. Every
 * comparison is a signed difference, and nothing is ever extrapolated further
 * than a few seconds from a reference, so neither wrap matters.
 */

#include <stdint.h>

/**
 * Frames to microseconds at `rate`, signed, rounded. The result must fit an
 * int32: about 35 minutes either way at 44.1 kHz, which differences of
 * seconds never approach.
 */
int32_t syncFramesToUs(int32_t frames, uint32_t rate);

/** Microseconds to frames at `rate`, signed, rounded. */
int32_t syncUsToFrames(int32_t us, uint32_t rate);

/**
 * This node's output: the local time at which output frame N plays.
 *
 * "Output frames" are everything ever written to the I2S driver since it was
 * installed -- audio, silence and corrections alike -- because the driver
 * fills its DMA buffers in exactly that order.
 */
class PlayoutClock {
public:
    void begin(uint32_t rate) { rate_ = rate; valid_ = false; }

    /** The timeline broke -- the DMA ran dry, or the driver was reinstalled. */
    void reset() { valid_ = false; }

    /**
     * A write that had to wait for a DMA buffer returned at `nowUs`, and the
     * next frame to be written, `nextFrame`, plays after `aheadFrames` more.
     *
     * The wait ends in the buffer-done interrupt, so the task can only see it
     * *late*, never early. An observation earlier than the prediction is
     * therefore taken outright; a later one is believed only an eighth at a
     * time, so a task that was preempted after waking moves the estimate by
     * microseconds, while a real difference in rate is still followed.
     */
    void observe(uint32_t nowUs, uint32_t nextFrame, uint32_t aheadFrames);

    bool valid() const { return valid_; }

    /** When output frame `frame` plays. Only near the last observation. */
    uint32_t playUs(uint32_t frame) const;

    /** Observations taken since begin(), for telemetry. */
    uint32_t observations = 0;

private:
    uint32_t rate_        = 44100;
    bool     valid_       = false;
    uint32_t anchorFrame_ = 0;
    uint32_t anchorUs_    = 0;
};

/**
 * The frames a node's output driver holds ahead of the next write, just after
 * a write that waited for a buffer: the one now playing (it has just started),
 * the rest of the ring, and whatever of its own buffer the write filled.
 */
uint32_t syncDmaAheadFrames(uint32_t framesWritten, uint32_t bufCount, uint32_t bufLen);

/**
 * The server's schedule, as seen from a client: the local time at which each
 * frame of the client's ring is due to play, fitted from the stamped packets.
 *
 * Each packet gives one point -- frame n (the ring index its block was pushed
 * at) is due at t -- and every point is late by its own transit. The line
 * through the earliest points is the schedule. Points are gathered in windows
 * (add() from the receive path, harvest() from the playing loop), each window
 * keeps its earliest, and the estimate is the earliest of the last few windows,
 * so a window with no fast packet in it costs nothing.
 *
 * The earliest-of-history rule would also hide a server whose schedule moved
 * *later* -- it re-armed, it played a jingle -- for as long as the old windows
 * last. So two windows in a row that land more than `shiftUs` after the
 * estimate replace it: one such window is a stretch of slow packets, two are a
 * new schedule. A schedule that moved earlier needs no rule; the minimum takes
 * it at once.
 *
 * Not thread-safe: the caller serialises add() against harvest().
 */
class ServerTimeline {
public:
    static const int HISTORY = 4;

    void begin(uint32_t rate, uint32_t minSamples, uint32_t maxAgeMs, int32_t shiftUs);

    /** Forget everything: a new stream, or numbering that no longer lines up. */
    void clear();

    /** One stamped packet: ring frame `frame` is due at local time `dueUs`. */
    void add(uint32_t frame, uint32_t dueUs);

    /** Close the current window. Windows with too few points are discarded. */
    void harvest(uint32_t nowMs);

    /** A usable estimate exists. */
    bool valid(uint32_t nowMs) const;

    /** When ring frame `frame` is due, by the earliest recent window. */
    uint32_t dueUs(uint32_t frame, uint32_t nowMs) const;

    /** Points in the open window, for telemetry. */
    uint32_t pending() const { return count_; }

private:
    struct Point { uint32_t frame; uint32_t us; uint32_t atMs; bool used; };

    uint32_t rate_       = 44100;
    uint32_t minSamples_ = 1;
    uint32_t maxAgeMs_   = 1000;
    int32_t  shiftUs_    = 4000;
    int      later_      = 0;    ///< consecutive windows beyond shiftUs_ late
    int      last_       = -1;   ///< the slot harvested last

    // The open window: its first point, and the earliest offset from it.
    uint32_t refFrame_ = 0;
    uint32_t refUs_    = 0;
    int32_t  minDelta_ = 0;
    uint32_t count_    = 0;

    Point hist_[HISTORY] = {};
    int   next_          = 0;
};

#endif  // SYNC_H
