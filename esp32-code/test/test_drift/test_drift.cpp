/**
 * Host-side tests for the CLIENT clock-drift controller.
 *
 * Run with:  pio test -e native      (or -e esp32s3 / -e esp32dev on a board)
 *
 * The interesting tests here are the closed-loop simulations at the bottom.
 * Drift is a slow effect -- the real one takes 18 minutes to empty a buffer --
 * so proving convergence on hardware costs the better part of an hour per
 * attempt. Simulated against the *measured* 30.5 ppm it costs milliseconds, and
 * the bench run afterwards becomes a confirmation rather than an exploration.
 *
 * The simulation is checked against reality before it is trusted: with the
 * controller switched off it must reproduce the 1.34 bytes/s slope recorded in
 * CHANGELOG.md for v0.1.0. If that assertion ever fails the model is wrong and
 * nothing below it means anything.
 */

#include <unity.h>

#include "drift.h"

// The client audio path these numbers describe; kept in step with config.h.
static const int SAMPLE_RATE   = 22050;
static const int BYTES_PER_SEC = SAMPLE_RATE * 2;   // 16-bit mono
static const int BATCH_SAMPLES = 128;               // CLIENT_BATCH
static const int BATCH_BYTES   = BATCH_SAMPLES * 2;
// DRIFT_TARGET_BYTES: the prefill minus what the I2S DMA ring holds once
// playback is running. The ring buffer only ever contains the remainder, which
// is what the controller can see -- steering to the full prefill was a real bug,
// caught on a C3 client, and test_target_is_the_ring_not_the_prefill pins it.
static const int TARGET        = 4000 - 2048;
static const int BUF_SIZE      = 8192;              // JITTER_BUF_SIZE
// Health margins: how close the ring may come to empty (underrun) or full
// (overflow). Not the DMA capacity -- the ring legitimately sits below that
// number now, because the DMA is holding the rest of the prefill.
static const double SAFE_MARGIN = 800.0;

static DriftController::Config defaultCfg() {
    DriftController::Config c;
    c.targetBytes   = TARGET;
    c.deadbandBytes = 400;
    c.kp            = 0.005f;
    c.maxRatePerSec = 5.0f;
    c.emaTauMs      = 4000.0f;
    c.settleMs      = 12000;
    return c;
}

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Controller behaviour
// ---------------------------------------------------------------------------

/** Hold the fill constant for `ms` and return the net corrections confirmed. */
static int runAt(DriftController &d, int fillBytes, uint32_t ms, uint32_t startMs = 0) {
    int net = 0;
    for (uint32_t t = startMs; (uint32_t)(t - startMs) < ms; t += 6) {
        DriftController::Correction c = d.update(t, fillBytes);
        d.confirm(c);
        net += (int)c;
    }
    return net;
}

static void test_first_update_only_seeds(void) {
    DriftController d;
    d.begin(defaultCfg(), 0);
    // A fill far from target on the very first sample is a restart, not drift:
    // the filter takes the reading and asks for nothing.
    TEST_ASSERT_EQUAL_INT(DriftController::NONE, d.update(0, 0));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.smoothedFill());
}

static void test_no_correction_inside_deadband(void) {
    DriftController d;
    d.begin(defaultCfg(), 0);
    // 399 bytes off target, held for a minute: the controller must not twitch.
    TEST_ASSERT_EQUAL_INT(0, runAt(d, TARGET - 399, 60000));
    TEST_ASSERT_EQUAL_UINT32(0, d.inserted);
    TEST_ASSERT_EQUAL_UINT32(0, d.dropped);
}

static void test_draining_buffer_asks_to_insert(void) {
    DriftController d;
    d.begin(defaultCfg(), 0);
    // Longer than settleMs: nothing is corrected inside the dead time.
    int net = runAt(d, TARGET - 1400, 25000);
    TEST_ASSERT_LESS_THAN_INT(0, net);
    TEST_ASSERT_TRUE(d.inserted > 0);
    TEST_ASSERT_EQUAL_UINT32(0, d.dropped);
}

static void test_filling_buffer_asks_to_drop(void) {
    DriftController d;
    d.begin(defaultCfg(), 0);
    int net = runAt(d, TARGET + 1400, 25000);
    TEST_ASSERT_GREATER_THAN_INT(0, net);
    TEST_ASSERT_TRUE(d.dropped > 0);
    TEST_ASSERT_EQUAL_UINT32(0, d.inserted);
}

static void test_rate_is_clamped(void) {
    DriftController d;
    DriftController::Config c = defaultCfg();
    d.begin(c, 0);
    // An error of 4000 bytes would ask for 18/s unclamped.
    runAt(d, 0, 40000);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -c.maxRatePerSec, d.rate());
}

static void test_unapplied_correction_stays_owed(void) {
    DriftController d;
    d.begin(defaultCfg(), 0);

    // Drive it until a correction is due, then refuse to apply it.
    DriftController::Correction c = DriftController::NONE;
    uint32_t t = 0;
    for (; t < 40000 && c == DriftController::NONE; t += 6) {
        c = d.update(t, TARGET - 1400);
    }
    TEST_ASSERT_EQUAL_INT(DriftController::INSERT, c);

    // Not confirmed: it must ask again, and nothing is counted.
    TEST_ASSERT_EQUAL_INT(DriftController::INSERT, d.update(t, TARGET - 1400));
    TEST_ASSERT_EQUAL_UINT32(0, d.inserted);

    // Confirmed once: the debt is paid and it does not immediately re-ask.
    d.confirm(DriftController::INSERT);
    TEST_ASSERT_EQUAL_UINT32(1, d.inserted);
    TEST_ASSERT_EQUAL_INT(DriftController::NONE, d.update(t + 6, TARGET - 1400));
}

static void test_reset_forgets_everything(void) {
    DriftController d;
    d.begin(defaultCfg(), 0);
    runAt(d, TARGET - 1400, 25000);
    TEST_ASSERT_TRUE(d.inserted > 0);

    d.reset(25000);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.rate());
    // Re-seeds on the next reading rather than reading a post-underrun refill
    // as a 3000-byte error.
    TEST_ASSERT_EQUAL_INT(DriftController::NONE, d.update(25000, TARGET - 3000));
    TEST_ASSERT_EQUAL_FLOAT((float)(TARGET - 3000), d.smoothedFill());
}

static void test_settle_window_suppresses_the_arming_transient(void) {
    DriftController d;
    d.begin(defaultCfg(), 0);

    // What arming actually looks like: playback starts with the full prefill in
    // the ring and an empty DMA ring, and the DMA takes its share within about
    // 50 ms. That step is 2,048 bytes and it is not drift.
    uint32_t t = 0;
    for (; t < 60; t += 6) { d.confirm(d.update(t, 4000)); }
    for (; t < 11000; t += 6) { d.confirm(d.update(t, TARGET)); }

    // Nothing corrected: the step was absorbed by the dead time, not chased.
    TEST_ASSERT_EQUAL_UINT32(0, d.inserted);
    TEST_ASSERT_EQUAL_UINT32(0, d.dropped);

    // And the filter was running throughout, so when the window closes the
    // estimate is essentially the settled level. Three time constants leaves
    // about 5% of the 2,048-byte step -- ~100 bytes, well inside the deadband,
    // so it cannot provoke a correction on its own.
    TEST_ASSERT_FLOAT_WITHIN(150.0f, (float)TARGET, d.smoothedFill());
}

static void test_natural_level_needs_no_correction(void) {
    // The target is the prefill minus what the DMA ring holds -- the level the
    // ring actually settles at. Steering to the prefill instead pulled a real
    // client 1,600 bytes above its natural level at nearly full rate for
    // minutes. Held at the natural level, the controller must do nothing at all.
    DriftController d;
    d.begin(defaultCfg(), 0);
    runAt(d, TARGET, 120000);
    TEST_ASSERT_EQUAL_UINT32(0, d.inserted);
    TEST_ASSERT_EQUAL_UINT32(0, d.dropped);
}

static void test_survives_the_millis_wrap(void) {
    DriftController d;
    const uint32_t nearWrap = 0xFFFFF000u;
    d.begin(defaultCfg(), nearWrap);
    // Straddle the wrap. A naive dt would go hugely positive or negative here
    // and either freeze the filter or dump a burst of corrections.
    int net = runAt(d, TARGET - 1400, 30000, nearWrap);
    TEST_ASSERT_LESS_THAN_INT(0, net);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -5.0f, d.rate());
}

static void test_a_stall_cannot_dump_credit(void) {
    DriftController d;
    d.begin(defaultCfg(), 0);
    runAt(d, TARGET - 1400, 30000);              // rate now clamped at -5/s
    uint32_t before = d.inserted;

    // A 10-second stall (mode change, a blocked write). At -5/s an unclamped dt
    // would owe 50 corrections at once; the clamp caps it at one batch's worth.
    DriftController::Correction c = d.update(40000, TARGET - 1400);
    d.confirm(c);
    TEST_ASSERT_TRUE(d.inserted - before <= 1);
}

// ---------------------------------------------------------------------------
// Closed loop against the measured drift
// ---------------------------------------------------------------------------

struct SimResult {
    double   finalFill;
    double   minFill;
    double   maxFill;
    double   slopeBytesPerSec;
    double   seconds;
    uint32_t inserted;
    uint32_t dropped;
    // Counted at the halfway mark. The controller does nothing at all until the
    // error has crossed the deadband, which takes ~300 s at 30 ppm, so a whole-
    // run average understates the rate it settles at. The second half is the
    // steady state, and that is the number that has to equal the drift.
    uint32_t insertedAtHalf;
    uint32_t droppedAtHalf;
};

/**
 * One client playing a stream from a source whose clock differs by `ppm`
 * (positive = the client consumes faster than the source produces, which is what
 * the two boards on the bench actually do).
 *
 * Each batch the client takes BATCH_BYTES out on its own clock, and the source
 * puts in whatever it produced during that much real time.
 */
static SimResult simulate(double ppm, double seconds, bool correct) {
    DriftController d;
    d.begin(defaultCfg(), 0);

    const double err      = ppm * 1e-6;
    const double dtReal   = (double)BATCH_BYTES / ((double)BYTES_PER_SEC * (1.0 + err));
    const double produced = (double)BYTES_PER_SEC * dtReal;

    double fill = (double)TARGET;
    double tMs  = 0.0;

    SimResult r;
    r.finalFill = fill;
    r.minFill   = fill;
    r.maxFill   = fill;
    r.slopeBytesPerSec = 0.0;
    r.seconds   = 0.0;
    r.inserted  = 0;
    r.dropped   = 0;
    r.insertedAtHalf = 0;
    r.droppedAtHalf  = 0;

    const long batches = (long)(seconds / dtReal);
    for (long i = 0; i < batches; i++) {
        if (i == batches / 2) {
            r.insertedAtHalf = d.inserted;
            r.droppedAtHalf  = d.dropped;
        }

        fill += produced;

        double consume = (double)BATCH_BYTES;
        if (correct) {
            DriftController::Correction c = d.update((uint32_t)tMs, (int)fill);
            // The caller can only apply a correction if the samples are there.
            if (c == DriftController::DROP && fill >= consume + 2.0) {
                consume += 2.0;
                d.confirm(c);
            } else if (c == DriftController::INSERT && fill >= consume - 2.0) {
                consume -= 2.0;
                d.confirm(c);
            }
        }

        fill -= consume;
        if (fill < 0.0)              fill = 0.0;                // underrun
        if (fill > (double)BUF_SIZE) fill = (double)BUF_SIZE;   // overflow

        if (fill < r.minFill) r.minFill = fill;
        if (fill > r.maxFill) r.maxFill = fill;
        tMs += dtReal * 1000.0;
    }

    r.finalFill        = fill;
    r.seconds          = tMs / 1000.0;
    r.slopeBytesPerSec = (fill - (double)TARGET) / r.seconds;
    r.inserted         = d.inserted;
    r.dropped          = d.dropped;
    return r;
}

/**
 * Model check. Uncorrected, the simulation must reproduce the slope measured on
 * hardware for v0.1.0: -1.34 bytes/s at -30.5 ppm. Everything below this test
 * depends on the model being right.
 */
static void test_model_reproduces_the_measured_slope(void) {
    SimResult r = simulate(30.5, 600.0, false);
    // Float, not double: Unity is built here without double support, and this
    // margin is four decimal places wider than float can lose.
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -1.34f, (float)r.slopeBytesPerSec);
}

static void test_uncorrected_buffer_empties_within_the_hour(void) {
    SimResult r = simulate(30.5, 3600.0, false);
    TEST_ASSERT_TRUE(r.minFill <= 0.0);   // it ran dry
}

static void test_corrected_buffer_holds_for_an_hour(void) {
    SimResult r = simulate(30.5, 3600.0, true);

    // The point of the exercise: an hour in, the buffer is still healthy --
    // clear of empty and clear of full, with no trend left.
    TEST_ASSERT_TRUE(r.minFill > SAFE_MARGIN);
    TEST_ASSERT_TRUE(r.maxFill < (double)BUF_SIZE - SAFE_MARGIN);

    // It parks below target: that offset is what generates the correction rate
    // the drift demands, and it is the documented cost of P-only control.
    TEST_ASSERT_TRUE(r.finalFill < (double)TARGET);
    TEST_ASSERT_TRUE(r.finalFill > (double)TARGET - 1000.0);

    // In the second half the loop has settled, and the correction rate must
    // then equal the drift it is cancelling: 30.5 ppm at 22.05 kHz is
    // 0.673 samples/s, so 1211 over the last 1800 s. Anything else and the
    // buffer would still be going somewhere.
    const uint32_t steady = r.inserted - r.insertedAtHalf;
    TEST_ASSERT_UINT32_WITHIN(60, 1211, steady);

    // Over the whole hour it is lower, because nothing happens until the error
    // clears the deadband -- ~300 s of untouched drift at the start. That is the
    // price of not chasing packet jitter, and it costs buffer depth, not audio.
    TEST_ASSERT_TRUE(r.inserted < steady * 2);
    TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
}

static void test_corrected_the_other_way_round(void) {
    // A client whose crystal is slow: the buffer fills instead, and the
    // controller must drop rather than insert.
    SimResult r = simulate(-30.5, 3600.0, true);
    TEST_ASSERT_TRUE(r.maxFill < (double)BUF_SIZE - SAFE_MARGIN);
    TEST_ASSERT_TRUE(r.minFill > SAFE_MARGIN);
    const uint32_t steady = r.dropped - r.droppedAtHalf;
    TEST_ASSERT_UINT32_WITHIN(60, 1211, steady);
    TEST_ASSERT_EQUAL_UINT32(0, r.inserted);
}

static void test_a_third_board_with_a_different_offset(void) {
    // The reason this is a controller and not a -30.5 ppm constant. Nothing is
    // retuned between these; the same defaults absorb all of them.
    static const double offsets[] = {-80.0, -12.0, 5.0, 60.0};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        SimResult r = simulate(offsets[i], 3600.0, true);
        TEST_ASSERT_TRUE(r.minFill > SAFE_MARGIN);
        TEST_ASSERT_TRUE(r.maxFill < (double)BUF_SIZE - SAFE_MARGIN);
    }
}

static void test_drift_beyond_authority_degrades_gracefully(void) {
    // 400 ppm is far outside anything two crystals do, and past the 5/s clamp
    // (227 ppm). The buffer loses the race, but the controller must still be
    // pulling at its maximum rather than winding up or oscillating.
    SimResult r = simulate(400.0, 600.0, true);
    TEST_ASSERT_TRUE(r.inserted > 0);
    TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
    TEST_ASSERT_TRUE(r.inserted > (uint32_t)(4.0 * 600.0 * 0.5));
}

// ---------------------------------------------------------------------------

static int runAllTests(void) {
    UNITY_BEGIN();

    RUN_TEST(test_first_update_only_seeds);
    RUN_TEST(test_no_correction_inside_deadband);
    RUN_TEST(test_draining_buffer_asks_to_insert);
    RUN_TEST(test_filling_buffer_asks_to_drop);
    RUN_TEST(test_rate_is_clamped);
    RUN_TEST(test_unapplied_correction_stays_owed);
    RUN_TEST(test_reset_forgets_everything);
    RUN_TEST(test_settle_window_suppresses_the_arming_transient);
    RUN_TEST(test_natural_level_needs_no_correction);
    RUN_TEST(test_survives_the_millis_wrap);
    RUN_TEST(test_a_stall_cannot_dump_credit);

    RUN_TEST(test_model_reproduces_the_measured_slope);
    RUN_TEST(test_uncorrected_buffer_empties_within_the_hour);
    RUN_TEST(test_corrected_buffer_holds_for_an_hour);
    RUN_TEST(test_corrected_the_other_way_round);
    RUN_TEST(test_a_third_board_with_a_different_offset);
    RUN_TEST(test_drift_beyond_authority_degrades_gracefully);

    return UNITY_END();
}

#ifdef ARDUINO

// Same tests on the board, for the same two reasons as test_jitter: there is no
// host compiler on this machine yet, and the controller runs in float -- which
// is hardware on the Xtensa boards and soft-float on a C3, so "it behaves the
// same on the target" is worth asserting rather than assuming.
#include <Arduino.h>

void setup() {
    delay(2000);   // let the host's serial monitor attach before output starts
    runAllTests();
}

void loop() {}

#else

int main(int, char **) {
    return runAllTests();
}

#endif
