/**
 * Host-side tests for the CLIENT receive path.
 *
 * Run with:  pio test -e native
 *
 * Every test here corresponds to something that either was a real bug in this
 * project or is an invariant the audio depends on. In particular:
 *   - a partial push must never shift 16-bit framing (test_*_framing_*)
 *   - a packet behind the last sequence number must resync, not report a
 *     65535-packet loss (test_seq_reordered_*)
 */

#include <unity.h>

#include <string.h>

#include "jitter.h"
#include "seqtracker.h"

// Small on purpose: 64 bytes means the wrap-around paths are reached in a few
// pushes instead of never.
static const int  BUF_SIZE = 64;
static uint8_t    storage[BUF_SIZE];
static JitterBuffer jb;

void setUp(void) {
    memset(storage, 0xAA, sizeof(storage));
    TEST_ASSERT_TRUE(jb.init(storage, BUF_SIZE));
}

void tearDown(void) {}

// ---------------------------------------------------------------------------
// JitterBuffer — construction and accounting
// ---------------------------------------------------------------------------

static void test_init_rejects_non_power_of_two(void) {
    JitterBuffer bad;
    TEST_ASSERT_FALSE(bad.init(storage, 48));
    TEST_ASSERT_FALSE(bad.init(storage, 0));
    TEST_ASSERT_FALSE(bad.init(nullptr, 64));
    TEST_ASSERT_TRUE(bad.init(storage, 32));
}

static void test_empty_buffer_accounting(void) {
    TEST_ASSERT_EQUAL_INT(0, jb.fill());
    // One byte is reserved so full and empty stay distinguishable.
    TEST_ASSERT_EQUAL_INT(BUF_SIZE - 1, jb.free());
}

static void test_fill_and_free_are_complementary(void) {
    uint8_t data[10] = {0};
    TEST_ASSERT_TRUE(jb.pushBlock(data, 10));
    TEST_ASSERT_EQUAL_INT(10, jb.fill());
    TEST_ASSERT_EQUAL_INT(BUF_SIZE - 1 - 10, jb.free());

    jb.advance(4);
    TEST_ASSERT_EQUAL_INT(6, jb.fill());
    TEST_ASSERT_EQUAL_INT(BUF_SIZE - 1 - 6, jb.free());
}

static void test_can_fill_to_capacity_but_not_beyond(void) {
    uint8_t data[BUF_SIZE] = {0};
    TEST_ASSERT_TRUE(jb.pushBlock(data, BUF_SIZE - 1));
    TEST_ASSERT_EQUAL_INT(BUF_SIZE - 1, jb.fill());
    TEST_ASSERT_EQUAL_INT(0, jb.free());
    TEST_ASSERT_FALSE(jb.pushBlock(data, 1));
}

// ---------------------------------------------------------------------------
// JitterBuffer — data integrity
// ---------------------------------------------------------------------------

static void test_push_peek_roundtrip(void) {
    uint8_t in[8]  = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t out[8] = {0};

    TEST_ASSERT_TRUE(jb.pushBlock(in, 8));
    TEST_ASSERT_TRUE(jb.peek(out, 8));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 8);
}

static void test_peek_does_not_consume(void) {
    uint8_t in[4]  = {9, 8, 7, 6};
    uint8_t out[4] = {0};

    TEST_ASSERT_TRUE(jb.pushBlock(in, 4));
    TEST_ASSERT_TRUE(jb.peek(out, 4));
    TEST_ASSERT_EQUAL_INT(4, jb.fill());          // still there
    TEST_ASSERT_TRUE(jb.peek(out, 4));            // and readable again
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 4);

    jb.advance(4);
    TEST_ASSERT_EQUAL_INT(0, jb.fill());
    TEST_ASSERT_FALSE(jb.peek(out, 4));
}

static void test_peek_fails_without_consuming_when_short(void) {
    uint8_t in[4]  = {1, 2, 3, 4};
    uint8_t out[8] = {0};

    TEST_ASSERT_TRUE(jb.pushBlock(in, 4));
    TEST_ASSERT_FALSE(jb.peek(out, 8));
    TEST_ASSERT_EQUAL_INT(4, jb.fill());
}

static void test_push_silence_writes_zeroes(void) {
    uint8_t in[4]  = {0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t out[8] = {0};

    TEST_ASSERT_TRUE(jb.pushBlock(in, 4));
    TEST_ASSERT_TRUE(jb.pushSilence(4));
    TEST_ASSERT_TRUE(jb.peek(out, 8));
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_UINT8(0xFF, out[i]);
    for (int i = 4; i < 8; i++) TEST_ASSERT_EQUAL_UINT8(0x00, out[i]);
}

static void test_wraparound_preserves_byte_order(void) {
    uint8_t pad[40] = {0};
    TEST_ASSERT_TRUE(jb.pushBlock(pad, 40));
    jb.advance(40);   // read pointer now at 40, write pointer at 40

    // 32 bytes from offset 40 in a 64-byte ring: 24 before the end, 8 after.
    uint8_t in[32];
    for (int i = 0; i < 32; i++) in[i] = (uint8_t)(i + 1);
    TEST_ASSERT_TRUE(jb.pushBlock(in, 32));

    uint8_t out[32] = {0};
    TEST_ASSERT_TRUE(jb.peek(out, 32));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 32);
}

// ---------------------------------------------------------------------------
// JitterBuffer — the framing invariant
// ---------------------------------------------------------------------------

/**
 * The bug this guards: a push that does not fit must change nothing at all.
 * The old byte-at-a-time version dropped whatever did not fit, and dropping an
 * odd byte count shifted every later 16-bit sample permanently.
 */
static void test_rejected_push_leaves_buffer_untouched(void) {
    uint8_t in[40];
    for (int i = 0; i < 40; i++) in[i] = (uint8_t)(i + 1);
    TEST_ASSERT_TRUE(jb.pushBlock(in, 40));

    const int fillBefore = jb.fill();
    uint8_t before[40] = {0};
    TEST_ASSERT_TRUE(jb.peek(before, 40));

    // 40 + 40 > 63, so this must be refused outright, not partially applied.
    uint8_t more[40];
    memset(more, 0x5A, sizeof(more));
    TEST_ASSERT_FALSE(jb.pushBlock(more, 40));

    TEST_ASSERT_EQUAL_INT(fillBefore, jb.fill());
    uint8_t after[40] = {0};
    TEST_ASSERT_TRUE(jb.peek(after, 40));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, after, 40);
}

static void test_rejected_silence_leaves_buffer_untouched(void) {
    uint8_t in[40];
    for (int i = 0; i < 40; i++) in[i] = (uint8_t)(i + 1);
    TEST_ASSERT_TRUE(jb.pushBlock(in, 40));

    TEST_ASSERT_FALSE(jb.pushSilence(40));
    TEST_ASSERT_EQUAL_INT(40, jb.fill());

    uint8_t out[40] = {0};
    TEST_ASSERT_TRUE(jb.peek(out, 40));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 40);
}

/**
 * End-to-end framing: stream int16 samples through the ring across several
 * wraps, with an overflow rejection in the middle, and confirm every sample
 * comes back with its two bytes in the right order.
 */
static void test_int16_framing_survives_wrap_and_overflow(void) {
    int16_t nextIn  = 0;
    int16_t nextOut = 0;

    for (int round = 0; round < 50; round++) {
        int16_t block[8];
        for (int i = 0; i < 8; i++) block[i] = (int16_t)(nextIn + i);

        if (jb.pushBlock((const uint8_t *)block, sizeof(block))) {
            nextIn += 8;
        }
        // If it did not fit, nextIn deliberately does not advance: the samples
        // were never accepted, so they are never expected on the way out.

        int16_t got[4];
        if (jb.peek((uint8_t *)got, sizeof(got))) {
            for (int i = 0; i < 4; i++) {
                TEST_ASSERT_EQUAL_INT16(nextOut + i, got[i]);
            }
            nextOut += 4;
            jb.advance(sizeof(got));
        }
    }

    TEST_ASSERT_TRUE(nextOut > 0);   // the test actually exercised the path
}

static void test_reset_empties_the_buffer(void) {
    uint8_t in[16] = {0};
    TEST_ASSERT_TRUE(jb.pushBlock(in, 16));
    jb.reset();
    TEST_ASSERT_EQUAL_INT(0, jb.fill());
    TEST_ASSERT_EQUAL_INT(BUF_SIZE - 1, jb.free());
}

// ---------------------------------------------------------------------------
// SeqTracker
// ---------------------------------------------------------------------------

static const uint16_t RESYNC_THRESHOLD = 64;
static const int      MAX_GAP_FILL     = 4;

static void test_seq_first_packet_is_accepted(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    SeqResult r = s.update(12345);   // any starting value, not just 0
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.resync);
}

static void test_seq_in_order_stream_reports_nothing(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    for (uint16_t i = 100; i < 200; i++) {
        SeqResult r = s.update(i);
        TEST_ASSERT_TRUE(r.accept);
        TEST_ASSERT_EQUAL_INT(0, r.fillPackets);
    }
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.dupe);
    TEST_ASSERT_EQUAL_UINT32(0, s.resync);
}

static void test_seq_exact_duplicate_is_dropped(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(10);
    SeqResult r = s.update(10);
    TEST_ASSERT_FALSE(r.accept);
    TEST_ASSERT_EQUAL_UINT32(1, s.dupe);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);

    // The stream continues normally afterwards.
    r = s.update(11);
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);
}

static void test_seq_single_loss_fills_one_packet(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(10);
    SeqResult r = s.update(12);   // 11 is missing
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(1, r.fillPackets);
    TEST_ASSERT_EQUAL_UINT32(1, s.lost);
}

static void test_seq_long_gap_is_counted_fully_but_fill_is_capped(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(10);
    SeqResult r = s.update(30);   // 19 packets missing, below the threshold
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(MAX_GAP_FILL, r.fillPackets);
    TEST_ASSERT_EQUAL_UINT32(19, s.lost);
}

static void test_seq_wraps_cleanly_at_65535(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(65534);
    SeqResult r = s.update(65535);
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);

    r = s.update(0);   // the wrap itself is an ordinary in-order step
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);

    r = s.update(1);
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.resync);
}

/**
 * The 65535 bug. A reordered frame arrives behind lastSeq; unsigned arithmetic
 * makes that look like a gap of ~65535. It must resync, not charge 65535 to
 * `lost` and splice in silence.
 */
static void test_seq_reordered_packet_resyncs_instead_of_reporting_huge_loss(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(500);
    s.update(501);
    SeqResult r = s.update(499);   // arrived late, out of order

    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);
    TEST_ASSERT_EQUAL_UINT32(1, s.resync);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);
}

static void test_seq_server_restart_resyncs(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(5000);
    SeqResult r = s.update(0);   // server rebooted, counter restarted

    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);
    TEST_ASSERT_EQUAL_UINT32(1, s.resync);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);

    // And it tracks the new baseline from there.
    r = s.update(1);
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);
    TEST_ASSERT_EQUAL_UINT32(1, s.resync);
}

static void test_seq_threshold_boundary(void) {
    // seq 0 -> 64 is a gap of 63, i.e. threshold - 1: still ordinary loss.
    SeqTracker loss(RESYNC_THRESHOLD, MAX_GAP_FILL);
    loss.update(0);
    SeqResult r = loss.update(RESYNC_THRESHOLD);
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_UINT32(RESYNC_THRESHOLD - 1, loss.lost);
    TEST_ASSERT_EQUAL_UINT32(0, loss.resync);
    TEST_ASSERT_EQUAL_INT(MAX_GAP_FILL, r.fillPackets);

    // seq 0 -> 65 is a gap of exactly the threshold: resync, nothing lost.
    SeqTracker sync(RESYNC_THRESHOLD, MAX_GAP_FILL);
    sync.update(0);
    r = sync.update(RESYNC_THRESHOLD + 1);
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_UINT32(0, sync.lost);
    TEST_ASSERT_EQUAL_UINT32(1, sync.resync);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);
}

static void test_seq_reset_forgets_the_stream(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(100);
    s.update(200);   // resync
    TEST_ASSERT_EQUAL_UINT32(1, s.resync);

    s.reset();
    TEST_ASSERT_EQUAL_UINT32(0, s.resync);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.dupe);

    // A wildly different sequence is now simply the first packet again.
    SeqResult r = s.update(9999);
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_UINT32(0, s.resync);
}

// ---------------------------------------------------------------------------

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_init_rejects_non_power_of_two);
    RUN_TEST(test_empty_buffer_accounting);
    RUN_TEST(test_fill_and_free_are_complementary);
    RUN_TEST(test_can_fill_to_capacity_but_not_beyond);

    RUN_TEST(test_push_peek_roundtrip);
    RUN_TEST(test_peek_does_not_consume);
    RUN_TEST(test_peek_fails_without_consuming_when_short);
    RUN_TEST(test_push_silence_writes_zeroes);
    RUN_TEST(test_wraparound_preserves_byte_order);

    RUN_TEST(test_rejected_push_leaves_buffer_untouched);
    RUN_TEST(test_rejected_silence_leaves_buffer_untouched);
    RUN_TEST(test_int16_framing_survives_wrap_and_overflow);
    RUN_TEST(test_reset_empties_the_buffer);

    RUN_TEST(test_seq_first_packet_is_accepted);
    RUN_TEST(test_seq_in_order_stream_reports_nothing);
    RUN_TEST(test_seq_exact_duplicate_is_dropped);
    RUN_TEST(test_seq_single_loss_fills_one_packet);
    RUN_TEST(test_seq_long_gap_is_counted_fully_but_fill_is_capped);
    RUN_TEST(test_seq_wraps_cleanly_at_65535);
    RUN_TEST(test_seq_reordered_packet_resyncs_instead_of_reporting_huge_loss);
    RUN_TEST(test_seq_server_restart_resyncs);
    RUN_TEST(test_seq_threshold_boundary);
    RUN_TEST(test_seq_reset_forgets_the_stream);

    return UNITY_END();
}
