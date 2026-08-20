#ifndef DRIFT_H
#define DRIFT_H

/**
 * Clock-drift correction for the CLIENT audio path.
 *
 * The source and every client run off their own crystal, and nothing
 * synchronises them. Measured over 600 s: the S3 consumed 30.5 ppm faster than
 * the WROOM produced, and an ESP32-C3 against that same WROOM consumed 57.7 ppm
 * faster -- 1.34 and 2.55 bytes/s out of the jitter buffer, emptying it in 18 and
 * 10 minutes of continuous play respectively.
 *
 * Two clients of one source, differing by a factor of nearly two. The absolute
 * number is a property of a particular pair of crystals at a particular
 * temperature and is not worth hard-coding; that was the argument in advance,
 * and the second board then made it out loud.
 *
 * So this is a controller, not a constant. It watches the buffer occupancy and
 * asks the caller to consume one extra sample now and then (buffer too full) or
 * one fewer (buffer draining). At 22.05 kHz, 30 ppm is 0.66 samples/s and 58 ppm
 * is 1.28 -- one edit every second or two. That is far below audibility and needs
 * no resampler, which is the whole reason this approach was chosen over an SRC.
 *
 * Pure logic, no Arduino or ESP-IDF, so the convergence behaviour can be
 * simulated on the host rather than discovered on a bench -- see
 * test/test_drift. It is also the reason the caller applies the correction: this
 * class never touches I2S or the ring buffer, it only decides.
 *
 * Control law: proportional, on the smoothed fill error, with a deadband.
 *
 *   - Proportional only. The plant is an integrator (a rate error accumulates
 *     into a level error), so the buffer parks at whatever offset produces
 *     exactly the correction rate the drift demands, and stays there:
 *
 *         steady-state depth = target - (deadband + required_rate / kp)
 *
 *     That is not a footnote, it is the design constraint. The gain decides how
 *     deep the buffer runs, and that depth has to survive a radio hiccup. With
 *     config.h's values a -58 ppm client parks about 320 bytes below target;
 *     at the gain this started with, four times gentler, it parked 660 bytes
 *     below and underran on an ordinary two-packet loss. Adding an integral term
 *     would recover the remaining offset and buy a wind-up failure mode in
 *     exchange, which is a bad trade for a buffer this deep.
 *   - The deadband is what stops the controller chasing packet-arrival jitter.
 *     Below it, nothing happens at all.
 *   - Loop time constant is 1/(2*kp) -- 25 s with config.h's gain -- against an
 *     input filter at 4 s and a disturbance that takes minutes to build the
 *     offset it corrects. Three well-separated timescales, so it cannot ring.
 *
 * The numbers above are illustrative; config.h holds the actual values and the
 * reasoning for each. Re-derive rather than trusting this comment if they move.
 */

#include <stdint.h>

class DriftController {
public:
    struct Config {
        /**
         * Bounds on the fill the controller steers towards, in bytes.
         *
         * The target is *measured*, not assumed: when the settle window closes,
         * the smoothed fill is taken as the target, clamped into this band. That
         * level is where the client naturally runs with the designed prefill, so
         * steering to it corrects drift and nothing else.
         *
         * Computing it instead was wrong on hardware. Prefill minus the DMA
         * ring's capacity assumes the ring sits permanently full; measured, it
         * holds about 1,880 of its 2,048 bytes, and the 350-byte error showed up
         * as a client settling that much shallower than intended -- which is
         * depth it cannot spare, as the underrun below shows.
         *
         * The band is what the arithmetic can justify: ring + DMA content is
         * conserved at the prefill, so the ring is somewhere between prefill
         * minus the DMA capacity and the prefill itself. The ceiling is set
         * where the DMA would be less than half full, which after a settle
         * window means something is wrong and the measurement is not to be
         * trusted.
         */
        int   targetBytes;      ///< floor, and the fallback before calibration
        int   targetCeilBytes;  ///< ceiling
        /** No correction at all while |error| is inside this, in bytes. */
        int   deadbandBytes;
        /** Corrections per second per byte of error outside the deadband. */
        float kp;
        /** Hard cap on correction rate, corrections per second. */
        float maxRatePerSec;
        /** Time constant of the fill low-pass, milliseconds. */
        float emaTauMs;
        /**
         * Dead time after a reset before any correction may be applied, ms.
         *
         * Playback arms with a full prefill and an empty DMA ring, and the ring
         * then takes its share in one step. That step is an artefact of starting
         * up, not drift, and correcting it wastes authority in the wrong
         * direction. The filter is still running during this window -- only the
         * correcting is held off -- so it starts from a settled estimate.
         */
        uint32_t settleMs;
    };

    /** Intent returned by update(). */
    enum Correction : int8_t {
        INSERT = -1,   ///< buffer draining: emit a sample without consuming one
        NONE   =  0,
        DROP   = +1    ///< buffer filling: consume a sample without emitting it
    };

    void begin(const Config &cfg, uint32_t nowMs);

    /**
     * Forget the accumulated state. Call whenever playback restarts -- after an
     * underrun re-arms the prefill gate, or on entry to CLIENT mode. The buffer
     * refill that follows is not drift and must not be integrated as if it were.
     */
    void reset(uint32_t nowMs);

    /**
     * Feed the current occupancy. Call once per output batch.
     *
     * Returns what the caller *should* do; it does not assume the caller can.
     * Nothing is counted and no credit is spent until confirm() says the
     * correction actually happened, so an I2S write that comes up short simply
     * leaves the correction outstanding for the next batch.
     */
    Correction update(uint32_t nowMs, int fillBytes);

    /** Report what was actually applied -- the value update() returned, or NONE. */
    void confirm(Correction applied);

    /** Smoothed occupancy, bytes. Telemetry only. */
    float smoothedFill() const { return ema_; }

    /**
     * The fill being steered to. Equal to Config::targetBytes until the settle
     * window closes and the measured level replaces it. Worth reporting: it is
     * the difference between a client holding its depth and one settling
     * somewhere shallower than anyone intended.
     */
    int target() const { return target_; }

    /**
     * Current correction rate, corrections per second, signed as Correction is.
     * This is the controller's own estimate of the drift, and once the loop is
     * closed it is the only in-band measure of it left: a corrected buffer no
     * longer has a slope to regress. rate/sampleRate is the drift in ppm.
     */
    float rate() const { return rate_; }

    /** Counters, for the [BENCH] telemetry line. */
    uint32_t inserted = 0;
    uint32_t dropped  = 0;

private:
    Config   cfg_{};
    int      target_  = 0;      ///< measured at the end of the settle window
    float    ema_     = 0.0f;
    float    rate_    = 0.0f;
    float    credit_  = 0.0f;   ///< fractional corrections owed, signed
    uint32_t lastMs_  = 0;
    uint32_t settleLeft_ = 0;   ///< ms of dead time remaining after a reset
    bool     seeded_  = false;

    // A stall (mode change, a long blocking write) must not hand the controller
    // a huge dt and let it spend a second's worth of credit in one batch.
    static const uint32_t MAX_DT_MS = 100;
};

#endif  // DRIFT_H
