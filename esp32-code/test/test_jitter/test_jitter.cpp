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

#include "blocks.h"
#include "holes.h"
#include "jitter.h"
#include "losstrace.h"
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

// Every frame goes out twice (ESPNOW_TX_COPIES). The copy of a frame that did
// arrive is dropped like any duplicate, but it was meant, so `dupe` -- the count
// of duplicates nobody sent on purpose -- must not move.
static void test_seq_intended_repeat_is_dropped_without_a_dupe(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    for (uint16_t i = 10; i < 20; i++) {
        TEST_ASSERT_TRUE(s.update(i).accept);
        TEST_ASSERT_FALSE(s.update(i, true).accept);
    }
    TEST_ASSERT_EQUAL_UINT32(0, s.dupe);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.resync);
}

// The point of sending twice: the original is lost, the copy arrives, and the
// block plays as if nothing happened -- no silence, nothing charged to `lost`.
static void test_seq_repeat_stands_in_for_a_lost_original(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(10);
    s.update(10, true);
    // 11's original lost in the air; its copy arrives.
    SeqResult r = s.update(11, true);
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(0, r.fillPackets);
    // Then 12 as normal, both copies.
    TEST_ASSERT_TRUE(s.update(12).accept);
    TEST_ASSERT_FALSE(s.update(12, true).accept);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.dupe);
}

// Both copies lost is an ordinary loss, found when the next block arrives.
static void test_seq_both_copies_lost_is_one_loss(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(10);
    s.update(10, true);
    SeqResult r = s.update(12);   // both copies of 11 gone
    TEST_ASSERT_TRUE(r.accept);
    TEST_ASSERT_EQUAL_INT(1, r.fillPackets);
    TEST_ASSERT_FALSE(s.update(12, true).accept);
    TEST_ASSERT_EQUAL_UINT32(1, s.lost);
    TEST_ASSERT_EQUAL_UINT32(0, s.dupe);
}

// Every packet also carries the block before it. A single loss is rebuilt from
// the next packet and played, so it must not stay on `lost` -- that counter is
// the holes, and the harness turns it into a loss percentage.
static void test_seq_recovered_block_is_not_lost(void) {
    SeqTracker s(RESYNC_THRESHOLD, MAX_GAP_FILL);
    s.update(10);
    SeqResult r = s.update(12);   // 11 missing, and 12 carries a copy of it
    TEST_ASSERT_EQUAL_INT(1, r.fillPackets);
    s.recovered(1);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);

    // Two missing, one rebuilt: one hole left.
    s.update(15);
    TEST_ASSERT_EQUAL_UINT32(2, s.lost);
    s.recovered(1);
    TEST_ASSERT_EQUAL_UINT32(1, s.lost);

    // Never below zero, whatever the caller claims.
    s.recovered(5);
    TEST_ASSERT_EQUAL_UINT32(0, s.lost);
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
// JitterBuffer::patch and HoleTable — a lost block written over its silence
// when a later packet carries it (D13)
// ---------------------------------------------------------------------------

static void test_patch_replaces_unread_silence_in_place(void) {
    const uint8_t a[4] = {1, 2, 3, 4}, b[4] = {5, 6, 7, 8}, lost[4] = {9, 10, 11, 12};
    TEST_ASSERT_TRUE(jb.pushBlock(a, 4));
    const int pos = jb.writePos();
    TEST_ASSERT_TRUE(jb.pushSilence(4));
    TEST_ASSERT_TRUE(jb.pushBlock(b, 4));

    TEST_ASSERT_TRUE(jb.patch(pos, lost, 4, 0));
    TEST_ASSERT_EQUAL_INT(12, jb.fill());   // nothing moved: timing is the silence's
    const uint8_t want[12] = {1, 2, 3, 4, 9, 10, 11, 12, 5, 6, 7, 8};
    uint8_t out[12];
    TEST_ASSERT_TRUE(jb.peek(out, 12));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, 12);
}

static void test_patch_refuses_what_has_played(void) {
    const uint8_t b[4] = {1, 2, 3, 4}, lost[4] = {9, 9, 9, 9};
    const int pos = jb.writePos();
    TEST_ASSERT_TRUE(jb.pushSilence(4));
    TEST_ASSERT_TRUE(jb.pushBlock(b, 4));
    jb.advance(4);   // the silence went out

    TEST_ASSERT_FALSE(jb.patch(pos, lost, 4, 0));
    uint8_t out[4];
    TEST_ASSERT_TRUE(jb.peek(out, 4));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, out, 4);   // and what is next is untouched
}

static void test_patch_refuses_a_block_the_reader_is_inside(void) {
    const uint8_t lost[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    const int pos = jb.writePos();
    TEST_ASSERT_TRUE(jb.pushSilence(8));
    jb.advance(4);   // half of it has played: patching now would splice mid-block
    TEST_ASSERT_FALSE(jb.patch(pos, lost, 8, 0));
}

static void test_patch_keeps_its_distance_from_the_reader(void) {
    const uint8_t pad[8] = {0}, lost[4] = {9, 9, 9, 9};
    TEST_ASSERT_TRUE(jb.pushBlock(pad, 8));
    const int pos = jb.writePos();
    TEST_ASSERT_TRUE(jb.pushSilence(4));
    TEST_ASSERT_FALSE(jb.patch(pos, lost, 4, 12));   // 8 bytes ahead, guard 12
    TEST_ASSERT_TRUE(jb.patch(pos, lost, 4, 8));     // exactly at the guard is fine
}

static void test_patch_refuses_what_was_never_written(void) {
    const uint8_t lost[4] = {9, 9, 9, 9};
    const int pos = jb.writePos();
    TEST_ASSERT_FALSE(jb.patch(pos, lost, 4, 0));    // nothing pushed there yet
    TEST_ASSERT_TRUE(jb.pushSilence(2));
    TEST_ASSERT_FALSE(jb.patch(pos, lost, 4, 0));    // only half of it
    TEST_ASSERT_FALSE(jb.patch(-1, lost, 4, 0));
    TEST_ASSERT_FALSE(jb.patch(BUF_SIZE, lost, 4, 0));
}

static void test_patch_wraps_around_the_end(void) {
    uint8_t pad[60] = {0};
    TEST_ASSERT_TRUE(jb.pushBlock(pad, 60));
    jb.advance(60);
    const int pos = jb.writePos();
    TEST_ASSERT_EQUAL_INT(60, pos);
    TEST_ASSERT_TRUE(jb.pushSilence(8));   // bytes 60..63, then 0..3

    const uint8_t lost[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    TEST_ASSERT_TRUE(jb.patch(pos, lost, 8, 0));
    uint8_t out[8];
    TEST_ASSERT_TRUE(jb.peek(out, 8));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(lost, out, 8);
}

static void test_holes_give_each_position_once(void) {
    HoleTable h;
    int pos = -1;
    h.add(100, 40);
    TEST_ASSERT_TRUE(h.take(100, &pos));
    TEST_ASSERT_EQUAL_INT(40, pos);
    TEST_ASSERT_FALSE(h.take(100, &pos));   // a second copy must not patch again
}

static void test_holes_only_know_what_was_added(void) {
    HoleTable h;
    int pos;
    TEST_ASSERT_FALSE(h.take(7, &pos));
    h.add(7, 12);
    h.clear();
    TEST_ASSERT_FALSE(h.take(7, &pos));
}

static void test_holes_a_lap_later_is_a_different_block(void) {
    HoleTable h;
    int pos;
    h.add(5, 12);
    TEST_ASSERT_FALSE(h.take(5 + HoleTable::SLOTS, &pos));   // same slot, not the same block
    h.add(5 + HoleTable::SLOTS, 20);                          // overwrites the slot
    TEST_ASSERT_FALSE(h.take(5, &pos));
    TEST_ASSERT_TRUE(h.take(5 + HoleTable::SLOTS, &pos));
    TEST_ASSERT_EQUAL_INT(20, pos);
}

static void test_holes_wrap_cleanly_at_65535(void) {
    HoleTable h;
    int pos;
    h.add(65535, 1);
    h.add(0, 2);
    TEST_ASSERT_TRUE(h.take((uint16_t)(3 - 4), &pos));   // seq - distance across the wrap
    TEST_ASSERT_EQUAL_INT(1, pos);
    TEST_ASSERT_TRUE(h.take(0, &pos));
    TEST_ASSERT_EQUAL_INT(2, pos);
}

// ---------------------------------------------------------------------------
// BlockStore and parity — rebuilding a block from the XOR of two (D13)
// ---------------------------------------------------------------------------

static const int BLK = 4;
static uint8_t   blockStorage[BlockStore::SLOTS * BLK];

static void blk(uint8_t *out, uint8_t v) {
    for (int i = 0; i < BLK; i++) out[i] = (uint8_t)(v + i);
}

static void test_blocks_hold_what_was_put(void) {
    BlockStore bs;
    TEST_ASSERT_TRUE(bs.init(blockStorage, BLK));
    uint8_t a[BLK];
    blk(a, 10);
    TEST_ASSERT_NULL(bs.get(7));
    bs.put(7, a);
    TEST_ASSERT_NOT_NULL(bs.get(7));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, bs.get(7), BLK);
    bs.clear();
    TEST_ASSERT_NULL(bs.get(7));
    TEST_ASSERT_FALSE(bs.init(nullptr, BLK));
}

static void test_blocks_a_lap_later_is_a_different_block(void) {
    BlockStore bs;
    bs.init(blockStorage, BLK);
    uint8_t a[BLK], b[BLK];
    blk(a, 1);
    blk(b, 2);
    bs.put(3, a);
    TEST_ASSERT_NULL(bs.get(3 + BlockStore::SLOTS));   // same slot, not that block
    bs.put(3 + BlockStore::SLOTS, b);
    TEST_ASSERT_NULL(bs.get(3));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, bs.get(3 + BlockStore::SLOTS), BLK);
}

static void test_xor_rebuilds_either_half(void) {
    uint8_t a[BLK], b[BLK], p[BLK], out[BLK];
    blk(a, 0x31);
    blk(b, 0xC7);
    xorBlocks(p, a, b, BLK);
    xorBlocks(out, p, b, BLK);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, out, BLK);
    xorBlocks(out, p, a, BLK);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, out, BLK);
}

static void test_parity_rebuilds_the_near_block(void) {
    // Packet 100 carries 99 ^ 89. 99 was lost -- a lone loss, the common
    // case -- and 89 arrived: 99 comes back from the very next packet.
    BlockStore bs;
    bs.init(blockStorage, BLK);
    uint8_t b89[BLK];
    blk(b89, 89);
    bs.put(89, b89);
    uint16_t missing = 0;
    const uint8_t *known = nullptr;
    TEST_ASSERT_TRUE(parityRebuildable(bs, 100, 11, &missing, &known));
    TEST_ASSERT_EQUAL_UINT16(99, missing);
    TEST_ASSERT_EQUAL_PTR(bs.get(89), known);
}

static void test_parity_rebuilds_the_far_block(void) {
    // A run took 89..93; packet 100 brings 99 ^ 89, and 99 arrived.
    BlockStore bs;
    bs.init(blockStorage, BLK);
    uint8_t b99[BLK];
    blk(b99, 99);
    bs.put(99, b99);
    uint16_t missing = 0;
    const uint8_t *known = nullptr;
    TEST_ASSERT_TRUE(parityRebuildable(bs, 100, 11, &missing, &known));
    TEST_ASSERT_EQUAL_UINT16(89, missing);
    TEST_ASSERT_EQUAL_PTR(bs.get(99), known);
}

static void test_parity_needs_exactly_one_missing(void) {
    BlockStore bs;
    bs.init(blockStorage, BLK);
    uint16_t missing;
    const uint8_t *known;
    TEST_ASSERT_FALSE(parityRebuildable(bs, 100, 11, &missing, &known));   // both missing
    uint8_t x[BLK];
    blk(x, 5);
    bs.put(99, x);
    bs.put(89, x);
    TEST_ASSERT_FALSE(parityRebuildable(bs, 100, 11, &missing, &known));   // nothing to do
}

static void test_parity_at_distance_one_is_nothing(void) {
    BlockStore bs;
    bs.init(blockStorage, BLK);
    uint8_t x[BLK];
    blk(x, 5);
    bs.put(98, x);
    uint16_t missing;
    const uint8_t *known;
    TEST_ASSERT_FALSE(parityRebuildable(bs, 100, 1, &missing, &known));
    TEST_ASSERT_FALSE(parityRebuildable(bs, 100, 0, &missing, &known));
}

static void test_parity_across_65535(void) {
    BlockStore bs;
    bs.init(blockStorage, BLK);
    uint8_t x[BLK];
    blk(x, 5);
    bs.put(65528, x);   // seq 3 - 11 + 65536
    uint16_t missing = 0;
    const uint8_t *known = nullptr;
    TEST_ASSERT_TRUE(parityRebuildable(bs, 3, 11, &missing, &known));
    TEST_ASSERT_EQUAL_UINT16(2, missing);
    TEST_ASSERT_EQUAL_PTR(bs.get(65528), known);
}

// ---------------------------------------------------------------------------
// LossTrace — which packets were missed, for scoring redundancy offline
// ---------------------------------------------------------------------------

// Static: a LossTrace is half a kilobyte, and the board runs these on loop()'s
// stack.
static LossTrace trace;

static void test_trace_records_each_outcome_in_order(void) {
    trace = LossTrace();
    trace.add(10, 0);
    trace.add(11, 0);
    trace.add(14, 2);   // 12 and 13 never came
    uint16_t start = 0;
    uint8_t  bits[2] = {0xFF, 0xFF};
    TEST_ASSERT_EQUAL_INT(5, trace.take(&start, bits, 64));
    TEST_ASSERT_EQUAL_UINT16(10, start);
    TEST_ASSERT_EQUAL_HEX8(0x0C, bits[0]);   // 10 11 12 13 14 = 0 0 1 1 0, LSB first
    TEST_ASSERT_EQUAL_INT(0, trace.take(&start, bits, 64));
    TEST_ASSERT_EQUAL_UINT32(0, trace.dropped);
}

static void test_trace_takes_in_pieces_without_a_seam(void) {
    trace = LossTrace();
    trace.add(100, 0);
    trace.add(103, 2);   // 101, 102 lost
    trace.add(104, 0);
    trace.add(106, 1);   // 105 lost
    uint16_t start = 0;
    uint8_t  bits[1];
    TEST_ASSERT_EQUAL_INT(3, trace.take(&start, bits, 3));
    TEST_ASSERT_EQUAL_UINT16(100, start);
    TEST_ASSERT_EQUAL_HEX8(0x06, bits[0]);   // 100 101 102 = 0 1 1
    TEST_ASSERT_EQUAL_INT(4, trace.take(&start, bits, 3 + 5));
    TEST_ASSERT_EQUAL_UINT16(103, start);
    TEST_ASSERT_EQUAL_HEX8(0x04, bits[0]);   // 103 104 105 106 = 0 0 1 0
    // And a new add carries on from 107.
    trace.add(107, 0);
    TEST_ASSERT_EQUAL_INT(1, trace.take(&start, bits, 8));
    TEST_ASSERT_EQUAL_UINT16(107, start);
}

static void test_trace_wraps_cleanly_at_65535(void) {
    trace = LossTrace();
    trace.add(65534, 0);
    trace.add(1, 2);    // 65535 and 0 lost
    uint16_t start = 0;
    uint8_t  bits[1];
    TEST_ASSERT_EQUAL_INT(4, trace.take(&start, bits, 8));
    TEST_ASSERT_EQUAL_UINT16(65534, start);
    TEST_ASSERT_EQUAL_HEX8(0x06, bits[0]);
    TEST_ASSERT_EQUAL_UINT32(0, trace.dropped);
}

static void test_trace_a_resync_starts_a_new_numbering(void) {
    // A packet that does not follow -- the server restarted -- is a break:
    // what was not taken is dropped, and the reader sees a new first seq.
    trace = LossTrace();
    trace.add(10, 0);
    trace.add(11, 0);
    trace.add(500, 0);
    TEST_ASSERT_EQUAL_UINT32(2, trace.dropped);
    uint16_t start = 0;
    uint8_t  bits[1];
    TEST_ASSERT_EQUAL_INT(1, trace.take(&start, bits, 8));
    TEST_ASSERT_EQUAL_UINT16(500, start);
    TEST_ASSERT_EQUAL_HEX8(0x00, bits[0]);
}

static void test_trace_a_reader_that_falls_behind_is_a_break(void) {
    trace = LossTrace();
    for (int s = 0; s < LossTrace::BITS; s++) trace.add((uint16_t)s, 0);
    TEST_ASSERT_EQUAL_INT(LossTrace::BITS, trace.pending());
    TEST_ASSERT_EQUAL_UINT32(0, trace.dropped);
    trace.add((uint16_t)(LossTrace::BITS + 1), 1);   // two more do not fit
    TEST_ASSERT_EQUAL_UINT32((uint32_t)LossTrace::BITS, trace.dropped);
    uint16_t start = 0;
    uint8_t  bits[1];
    TEST_ASSERT_EQUAL_INT(2, trace.take(&start, bits, 8));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)LossTrace::BITS, start);
    TEST_ASSERT_EQUAL_HEX8(0x01, bits[0]);
}

static void test_trace_bits_survive_the_ring_wrapping(void) {
    // Take most of a ring, then add past its end: every 7th packet lost.
    trace = LossTrace();
    uint16_t seq = 1000;
    for (int i = 0; i < LossTrace::BITS - 10; i++) trace.add(seq++, 0);
    static uint8_t bits[LossTrace::BITS / 8];
    uint16_t start = 0;
    TEST_ASSERT_EQUAL_INT(LossTrace::BITS - 10, trace.take(&start, bits, LossTrace::BITS));
    const uint16_t first = seq;
    for (int i = 0; i < 100; i++) {
        if (i % 7 == 6) { seq++; trace.add(seq++, 1); i++; }
        else            trace.add(seq++, 0);
    }
    const int n = trace.take(&start, bits, LossTrace::BITS);
    TEST_ASSERT_EQUAL_UINT16(first, start);
    TEST_ASSERT_EQUAL_INT((int)(uint16_t)(seq - first), n);
    for (int k = 0; k < n; k++) {
        const bool lost = (bits[k / 8] >> (k % 8)) & 1;
        TEST_ASSERT_EQUAL_MESSAGE(k % 7 == 6, lost, "outcome out of place after the wrap");
    }
    TEST_ASSERT_EQUAL_UINT32(0, trace.dropped);
}

static void test_trace_reset_forgets_without_counting(void) {
    trace = LossTrace();
    trace.add(10, 0);
    trace.reset();
    TEST_ASSERT_EQUAL_INT(0, trace.pending());
    trace.add(20, 3);   // a first packet with losses before it: they are recorded
    uint16_t start = 0;
    uint8_t  bits[1];
    TEST_ASSERT_EQUAL_INT(4, trace.take(&start, bits, 8));
    TEST_ASSERT_EQUAL_UINT16(17, start);
    TEST_ASSERT_EQUAL_HEX8(0x07, bits[0]);
    TEST_ASSERT_EQUAL_UINT32(0, trace.dropped);
}

// ---------------------------------------------------------------------------

static int runAllTests(void) {
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
    RUN_TEST(test_seq_intended_repeat_is_dropped_without_a_dupe);
    RUN_TEST(test_seq_repeat_stands_in_for_a_lost_original);
    RUN_TEST(test_seq_both_copies_lost_is_one_loss);
    RUN_TEST(test_seq_recovered_block_is_not_lost);
    RUN_TEST(test_seq_single_loss_fills_one_packet);
    RUN_TEST(test_seq_long_gap_is_counted_fully_but_fill_is_capped);
    RUN_TEST(test_seq_wraps_cleanly_at_65535);
    RUN_TEST(test_seq_reordered_packet_resyncs_instead_of_reporting_huge_loss);
    RUN_TEST(test_seq_server_restart_resyncs);
    RUN_TEST(test_seq_threshold_boundary);
    RUN_TEST(test_seq_reset_forgets_the_stream);

    RUN_TEST(test_patch_replaces_unread_silence_in_place);
    RUN_TEST(test_patch_refuses_what_has_played);
    RUN_TEST(test_patch_refuses_a_block_the_reader_is_inside);
    RUN_TEST(test_patch_keeps_its_distance_from_the_reader);
    RUN_TEST(test_patch_refuses_what_was_never_written);
    RUN_TEST(test_patch_wraps_around_the_end);
    RUN_TEST(test_holes_give_each_position_once);
    RUN_TEST(test_holes_only_know_what_was_added);
    RUN_TEST(test_holes_a_lap_later_is_a_different_block);
    RUN_TEST(test_holes_wrap_cleanly_at_65535);

    RUN_TEST(test_blocks_hold_what_was_put);
    RUN_TEST(test_blocks_a_lap_later_is_a_different_block);
    RUN_TEST(test_xor_rebuilds_either_half);
    RUN_TEST(test_parity_rebuilds_the_near_block);
    RUN_TEST(test_parity_rebuilds_the_far_block);
    RUN_TEST(test_parity_needs_exactly_one_missing);
    RUN_TEST(test_parity_at_distance_one_is_nothing);
    RUN_TEST(test_parity_across_65535);

    RUN_TEST(test_trace_records_each_outcome_in_order);
    RUN_TEST(test_trace_takes_in_pieces_without_a_seam);
    RUN_TEST(test_trace_wraps_cleanly_at_65535);
    RUN_TEST(test_trace_a_resync_starts_a_new_numbering);
    RUN_TEST(test_trace_a_reader_that_falls_behind_is_a_break);
    RUN_TEST(test_trace_bits_survive_the_ring_wrapping);
    RUN_TEST(test_trace_reset_forgets_without_counting);

    return UNITY_END();
}

#ifdef ARDUINO

// Same tests, run on the board. Useful when there is no host compiler, and as a
// check that the logic behaves the same on Xtensa as it does on the PC.
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
