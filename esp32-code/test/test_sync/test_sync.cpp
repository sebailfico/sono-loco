/**
 * Host-side tests for playing in time (lib/sync).
 *
 * Run with:  pio test -e native      (or -e esp32dev / -e esp32s3 on a board)
 *
 * What these pin down is arithmetic that fails silently. An output clock read
 * a buffer off, or a timeline that believes a slow packet, costs nothing
 * visible in any counter: the node plays, cleanly, a few milliseconds away
 * from everyone else -- which is exactly the echo this library exists to
 * remove, and a microphone is the only other instrument that would catch it.
 *
 * Every test runs its clock across the 32-bit microsecond wrap at least once,
 * because a node reaches it every 71 minutes.
 */

#include <stdint.h>
#include <stdlib.h>
#include <unity.h>

#include "sync.h"

static const uint32_t RATE = 44100;

void setUp(void) {}
void tearDown(void) {}

// A deterministic generator, so a failure reproduces.
static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % n;
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

void test_frames_and_microseconds(void) {
    TEST_ASSERT_EQUAL_INT32(1000000, syncFramesToUs(44100, RATE));
    TEST_ASSERT_EQUAL_INT32(2585, syncFramesToUs(114, RATE));      // one mesh block
    TEST_ASSERT_EQUAL_INT32(-2585, syncFramesToUs(-114, RATE));    // symmetric
    TEST_ASSERT_EQUAL_INT32(0, syncFramesToUs(0, RATE));
    TEST_ASSERT_EQUAL_INT32(44100, syncUsToFrames(1000000, RATE));
    TEST_ASSERT_EQUAL_INT32(-441, syncUsToFrames(-10000, RATE));
    // 35 minutes either way still fits.
    TEST_ASSERT_EQUAL_INT32(2100000000, syncFramesToUs(92610000, RATE));
}

/**
 * Just after a write that waited: the buffer now playing has just started, the
 * rest of the ring is full, and the write's own buffer holds what it wrote.
 * With 128-frame batches into 256-frame buffers every wait ends half a buffer
 * in; a write that ends on a boundary has filled its buffer, not begun one.
 */
void test_dma_ahead_after_a_wait(void) {
    TEST_ASSERT_EQUAL_UINT32(7 * 256 + 128, syncDmaAheadFrames(1000 * 256 + 128, 8, 256));
    TEST_ASSERT_EQUAL_UINT32(8 * 256, syncDmaAheadFrames(1000 * 256, 8, 256));
    TEST_ASSERT_EQUAL_UINT32(3 * 256 + 1, syncDmaAheadFrames(4 * 256 + 1, 4, 256));
}

// ---------------------------------------------------------------------------
// PlayoutClock
// ---------------------------------------------------------------------------

/**
 * The truth: output frame w plays at t0 + w / rate (ppm slower if given). An
 * observation is that, seen by a task woken `latency` after the interrupt.
 */
struct Output {
    uint32_t t0;
    double   ppm;
    uint32_t playUs(uint32_t w) const {
        return t0 + (uint32_t)(int64_t)((double)w * 1e6 / RATE * (1.0 + ppm * 1e-6));
    }
};

void test_clock_reads_the_output_through_wake_latency(void) {
    Output out{0xFFFFFFFFu - 3000000u, 0.0};   // wraps 3 s in
    PlayoutClock c;
    c.begin(RATE);
    TEST_ASSERT_FALSE(c.valid());

    uint32_t w = 100000;   // past the DMA depth: the model's frame index is unsigned
    int maxErr = 0;
    for (int i = 0; i < 2000; i++) {   // 2000 waits, 11.6 s at one per 256 frames
        w += 256;
        const uint32_t ahead = 7 * 256 + 128;
        // The interrupt fires when frame w - ahead starts; the task sees it later.
        uint32_t latency = rnd(60);                  // usually tens of us
        if (rnd(50) == 0) latency = 200 + rnd(1500);  // now and then, preempted
        const uint32_t seen = out.playUs(w - ahead) + latency;
        c.observe(seen, w, ahead);
        if (i < 20) continue;   // a few waits to find the floor
        const int err = abs((int32_t)(c.playUs(w + 100) - out.playUs(w + 100)));
        if (err > maxErr) maxErr = err;
    }
    TEST_ASSERT_TRUE(c.valid());
    // The floor of the latency, not its average: a preempted wait moves the
    // estimate by an eighth, and the next ordinary one takes it back.
    TEST_ASSERT_LESS_THAN_INT(250, maxErr);
}

void test_clock_follows_an_output_slower_than_nominal(void) {
    // The I2S divider does not make exactly 44.1 kHz; 300 ppm is generous.
    Output out{0x7FFF0000u, 300.0};
    PlayoutClock c;
    c.begin(RATE);
    uint32_t w = 100000;
    int maxErr = 0;
    for (int i = 0; i < 3000; i++) {
        w += 256;
        c.observe(out.playUs(w - 1920) + rnd(40), w, 1920);
        if (i < 50) continue;
        const int err = abs((int32_t)(c.playUs(w) - out.playUs(w)));
        if (err > maxErr) maxErr = err;
    }
    TEST_ASSERT_LESS_THAN_INT(100, maxErr);
}

void test_clock_takes_an_earlier_observation_outright(void) {
    PlayoutClock c;
    c.begin(RATE);
    c.observe(1000000, 5000, 1920);
    const uint32_t before = c.playUs(5000);
    c.observe(1000000 - 3000, 5000, 1920);   // the same frame, 3 ms sooner
    TEST_ASSERT_EQUAL_INT32(-3000, (int32_t)(c.playUs(5000) - before));
    c.reset();
    TEST_ASSERT_FALSE(c.valid());
}

// ---------------------------------------------------------------------------
// ServerTimeline
// ---------------------------------------------------------------------------

/**
 * A stream of stamped packets: ring frame n is truly due at line(n), and every
 * packet reports it late by its transit beyond the fastest -- never early.
 */
struct Line {
    uint32_t t0;
    double   ppm;   // the server's clock against this one
    uint32_t dueUs(uint32_t n) const {
        return t0 + (uint32_t)(int64_t)((double)n * 1e6 / RATE * (1.0 + ppm * 1e-6));
    }
};

static uint32_t transitExtra() {
    const uint32_t r = rnd(100);
    if (r < 20) return rnd(30);             // crossed as fast as anything does
    if (r < 90) return 100 + rnd(2000);     // waited behind the burst
    return 5000 + rnd(40000);               // the Bluetooth link had the radio
}

void test_timeline_finds_the_line_under_the_transit(void) {
    const Line line{0xFFFFFFFFu - 500000u, 0.0};   // wraps half a second in
    ServerTimeline tl;
    tl.begin(RATE, 16, 1000, 4000);
    uint32_t n = 50000, ms = 0;
    for (int w = 0; w < 20; w++) {        // 5 s of 250 ms windows
        for (int p = 0; p < 97; p++) {
            tl.add(n, line.dueUs(n) + transitExtra());
            n += 114;
        }
        ms += 250;
        tl.harvest(ms);
    }
    TEST_ASSERT_TRUE(tl.valid(ms));
    const int err = (int32_t)(tl.dueUs(n + 4000, ms) - line.dueUs(n + 4000));
    TEST_ASSERT_TRUE(err >= 0);           // it can only err late
    TEST_ASSERT_LESS_THAN_INT(40, err);
}

/**
 * A window in which every packet waited -- a stretch where the server's
 * Bluetooth link held the radio -- must not drag the estimate later: the
 * earlier windows still stand.
 */
void test_timeline_ignores_a_window_of_slow_packets(void) {
    const Line line{123456789u, 0.0};
    ServerTimeline tl;
    tl.begin(RATE, 16, 1000, 4000);
    uint32_t n = 0, ms = 1000;
    for (int w = 0; w < 3; w++) {
        for (int p = 0; p < 97; p++, n += 114) tl.add(n, line.dueUs(n) + rnd(20));
        tl.harvest(ms += 250);
    }
    for (int p = 0; p < 97; p++, n += 114) tl.add(n, line.dueUs(n) + 8000 + rnd(20000));
    tl.harvest(ms += 250);
    const int err = (int32_t)(tl.dueUs(n, ms) - line.dueUs(n));
    TEST_ASSERT_LESS_THAN_INT(25, err);
}

/** The server's crystal is not this one's: the line has its own slope. */
void test_timeline_follows_the_servers_clock(void) {
    const Line line{0xFFF00000u, -60.0};
    ServerTimeline tl;
    tl.begin(RATE, 16, 1000, 4000);
    uint32_t n = 0, ms = 0;
    int maxErr = 0;
    for (int w = 0; w < 400; w++) {       // 100 s
        for (int p = 0; p < 97; p++, n += 114) tl.add(n, line.dueUs(n) + transitExtra());
        tl.harvest(ms += 250);
        if (w < 4) continue;
        const int err = abs((int32_t)(tl.dueUs(n, ms) - line.dueUs(n)));
        if (err > maxErr) maxErr = err;
    }
    // Four windows of history is one second: 60 ppm of it is 60 us of lag at
    // worst, on top of the transit floor.
    TEST_ASSERT_LESS_THAN_INT(120, maxErr);
}

void test_timeline_needs_enough_points_and_forgets(void) {
    ServerTimeline tl;
    tl.begin(RATE, 16, 1000, 4000);
    for (uint32_t i = 0; i < 15; i++) tl.add(i * 114, 1000000 + i * 2585);
    tl.harvest(100);
    TEST_ASSERT_FALSE(tl.valid(100));     // 15 points: not a window

    for (uint32_t i = 0; i < 16; i++) tl.add(i * 114, 1000000 + i * 2585);
    tl.harvest(200);
    TEST_ASSERT_TRUE(tl.valid(200));
    TEST_ASSERT_TRUE(tl.valid(1200));
    TEST_ASSERT_FALSE(tl.valid(1201));    // older than maxAge

    for (uint32_t i = 0; i < 16; i++) tl.add(i * 114, 1000000 + i * 2585);
    tl.harvest(300);
    tl.clear();
    TEST_ASSERT_FALSE(tl.valid(300));
}

/**
 * The server re-armed and now plays 30 ms later. One late window could be a
 * stretch of slow packets and must not move the estimate; the second in a row
 * is the new schedule, adopted at once rather than a second later when the
 * old windows age out.
 */
void test_timeline_adopts_a_schedule_that_moved_later(void) {
    Line line{0x10000000u, 0.0};
    ServerTimeline tl;
    tl.begin(RATE, 16, 1000, 4000);
    uint32_t n = 0, ms = 0;
    for (int w = 0; w < 4; w++) {
        for (int p = 0; p < 97; p++, n += 114) tl.add(n, line.dueUs(n) + rnd(20));
        tl.harvest(ms += 250);
    }
    line.t0 += 30000;
    for (int p = 0; p < 97; p++, n += 114) tl.add(n, line.dueUs(n) + rnd(20));
    tl.harvest(ms += 250);
    TEST_ASSERT_TRUE((int32_t)(line.dueUs(n) - tl.dueUs(n, ms)) > 29000);   // still the old
    for (int p = 0; p < 97; p++, n += 114) tl.add(n, line.dueUs(n) + rnd(20));
    tl.harvest(ms += 250);
    TEST_ASSERT_LESS_THAN_INT(25, abs((int32_t)(tl.dueUs(n, ms) - line.dueUs(n))));
}

/** A schedule that moved earlier is simply the new minimum. */
void test_timeline_adopts_a_schedule_that_moved_earlier(void) {
    Line line{0x10000000u, 0.0};
    ServerTimeline tl;
    tl.begin(RATE, 16, 1000, 4000);
    uint32_t n = 0, ms = 0;
    for (int w = 0; w < 4; w++) {
        for (int p = 0; p < 97; p++, n += 114) tl.add(n, line.dueUs(n) + rnd(20));
        tl.harvest(ms += 250);
    }
    line.t0 -= 30000;
    for (int p = 0; p < 97; p++, n += 114) tl.add(n, line.dueUs(n) + rnd(20));
    tl.harvest(ms += 250);
    TEST_ASSERT_LESS_THAN_INT(25, abs((int32_t)(tl.dueUs(n, ms) - line.dueUs(n))));
}

// ---------------------------------------------------------------------------

int runAllTests(void) {
    UNITY_BEGIN();

    RUN_TEST(test_frames_and_microseconds);
    RUN_TEST(test_dma_ahead_after_a_wait);

    RUN_TEST(test_clock_reads_the_output_through_wake_latency);
    RUN_TEST(test_clock_follows_an_output_slower_than_nominal);
    RUN_TEST(test_clock_takes_an_earlier_observation_outright);

    RUN_TEST(test_timeline_finds_the_line_under_the_transit);
    RUN_TEST(test_timeline_ignores_a_window_of_slow_packets);
    RUN_TEST(test_timeline_follows_the_servers_clock);
    RUN_TEST(test_timeline_needs_enough_points_and_forgets);
    RUN_TEST(test_timeline_adopts_a_schedule_that_moved_later);
    RUN_TEST(test_timeline_adopts_a_schedule_that_moved_earlier);

    return UNITY_END();
}

#ifdef ARDUINO

// Same tests on the board, as the other suites: there is no host compiler on
// this machine yet.
#include <Arduino.h>

void setup() {
    delay(2000);   // let the host's serial monitor attach before output starts
    runAllTests();
}

void loop() {}

#else

int main(void) { return runAllTests(); }

#endif
