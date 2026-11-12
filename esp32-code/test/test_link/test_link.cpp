/**
 * Host-side tests for the two-chip server's wire (lib/link, D18).
 *
 * Run with:  pio test -e native -f test_link
 *
 * A node that never decodes a frame never becomes a server and says nothing
 * about why, so every way a UART can mangle the stream -- garbage before the
 * first frame, a byte lost in the middle of one, the other board rebooting
 * half-way through a frame -- is played here, on the host.
 */

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "link.h"

void setUp(void) {}
void tearDown(void) {}

// A byte stream assembled from frames and noise, and what came out of it.
static uint8_t stream[8 * LINK_FRAME_MAX];
static size_t  streamLen;

static void add(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len) {
    const size_t n = linkEncode(stream + streamLen, sizeof(stream) - streamLen, type, seq, payload, len);
    TEST_ASSERT_TRUE(n > 0);
    streamLen += n;
}

static void addBytes(const uint8_t *b, size_t n) {
    memcpy(stream + streamLen, b, n);
    streamLen += n;
}

struct Seen {
    uint8_t  type, seq;
    uint16_t len;
    uint8_t  first;   // the payload's first byte, or 0
};
static Seen   seen[32];
static size_t seenCount;

/** Feed the stream in pieces of `piece` bytes, collecting every frame. */
static void decode(LinkDecoder &d, size_t piece) {
    seenCount = 0;
    size_t off = 0;
    while (off < streamLen) {
        size_t n = streamLen - off < piece ? streamLen - off : piece;
        const uint8_t *p = stream + off;
        off += n;
        while (true) {
            LinkFrame f;
            bool      got;
            const size_t used = d.push(p, n, &f, &got);
            p += used;
            n -= used;
            if (got && seenCount < 32) {
                seen[seenCount++] = {f.type, f.seq, f.len, f.len ? f.payload[0] : (uint8_t)0};
            }
            if (!got && n == 0) break;
        }
    }
}

static void fill(uint8_t *b, size_t n, uint8_t start) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(start + i);
}

// ---------------------------------------------------------------------------

void test_crc_is_ccitt_false(void) {
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX16(0x29B1, linkCrc16(check, sizeof(check)));
}

void test_one_frame_comes_back_whole(void) {
    uint8_t payload[100];
    fill(payload, sizeof(payload), 7);
    streamLen = 0;
    add(LINK_AUDIO, 42, payload, sizeof(payload));

    LinkDecoder d;
    LinkFrame   f;
    bool        got;
    const size_t used = d.push(stream, streamLen, &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(streamLen, used);
    TEST_ASSERT_EQUAL(LINK_AUDIO, f.type);
    TEST_ASSERT_EQUAL(42, f.seq);
    TEST_ASSERT_EQUAL(100, f.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, f.payload, 100);
    TEST_ASSERT_EQUAL(1, d.frames);
    TEST_ASSERT_EQUAL(0, d.crcErrors + d.badLength + d.skipped);
}

/** A UART hands over whatever it has: every split must give the same frames. */
void test_frames_survive_any_split(void) {
    uint8_t a[300], b[1], c[LINK_MAX_PAYLOAD];
    fill(a, sizeof(a), 1);
    b[0] = 0x55;
    fill(c, sizeof(c), 9);
    streamLen = 0;
    add(LINK_AUDIO, 0, a, sizeof(a));
    add(LINK_STATE, 1, b, sizeof(b));
    add(LINK_CMD, 2, nullptr, 0);
    add(LINK_AUDIO, 3, c, sizeof(c));

    const size_t pieces[] = {1, 2, 3, 7, 64, 1000, sizeof(stream)};
    for (size_t piece : pieces) {
        LinkDecoder d;
        decode(d, piece);
        TEST_ASSERT_EQUAL_MESSAGE(4, seenCount, "every frame, at every split");
        TEST_ASSERT_EQUAL(300, seen[0].len);
        TEST_ASSERT_EQUAL(1, seen[1].len);
        TEST_ASSERT_EQUAL(0x55, seen[1].first);
        TEST_ASSERT_EQUAL(0, seen[2].len);
        TEST_ASSERT_EQUAL(LINK_CMD, seen[2].type);
        TEST_ASSERT_EQUAL(LINK_MAX_PAYLOAD, seen[3].len);
        TEST_ASSERT_EQUAL(0, d.seqGaps);
        TEST_ASSERT_EQUAL(0, d.crcErrors + d.badLength + d.skipped);
    }
}

/** The other board's ROM banner on the same pins, before and between frames. */
void test_noise_before_and_between_frames_is_skipped(void) {
    const char *banner = "ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET)\r\n";
    uint8_t     p[20];
    fill(p, sizeof(p), 0);
    streamLen = 0;
    addBytes((const uint8_t *)banner, strlen(banner));
    add(LINK_AUDIO, 0, p, sizeof(p));
    addBytes((const uint8_t *)"\xA5\xA5\x5A", 1);   // a stray first sync byte
    add(LINK_AUDIO, 1, p, sizeof(p));

    LinkDecoder d;
    decode(d, 5);
    TEST_ASSERT_EQUAL(2, seenCount);
    TEST_ASSERT_EQUAL(strlen(banner) + 1, d.skipped);
    TEST_ASSERT_EQUAL(0, d.seqGaps);
}

/** A byte flipped in a payload costs that frame, and only that frame. */
void test_a_corrupt_frame_is_dropped_and_the_next_one_kept(void) {
    uint8_t p[64];
    fill(p, sizeof(p), 3);
    streamLen = 0;
    add(LINK_AUDIO, 0, p, sizeof(p));
    const size_t second = streamLen;
    add(LINK_AUDIO, 1, p, sizeof(p));
    add(LINK_AUDIO, 2, p, sizeof(p));
    stream[second + LINK_HEADER_BYTES + 10] ^= 0x40;

    LinkDecoder d;
    decode(d, 17);
    TEST_ASSERT_EQUAL(2, seenCount);
    TEST_ASSERT_EQUAL(0, seen[0].seq);
    TEST_ASSERT_EQUAL(2, seen[1].seq);
    TEST_ASSERT_EQUAL(1, d.crcErrors);
    TEST_ASSERT_EQUAL(1, d.seqGaps);
}

/**
 * The front end rebooting half-way through a frame: a header that promises
 * more than ever comes, then whole frames. The frames inside the bytes the
 * broken header claimed must not be swallowed with it.
 */
void test_a_cut_off_frame_does_not_swallow_the_ones_after_it(void) {
    uint8_t big[LINK_MAX_PAYLOAD], small[40];
    fill(big, sizeof(big), 0);
    fill(small, sizeof(small), 100);
    streamLen = 0;
    add(LINK_AUDIO, 0, big, sizeof(big));
    streamLen -= sizeof(big) - 30;   // cut: header and 28 bytes of payload survive
    add(LINK_STATE, 0, small, sizeof(small));
    add(LINK_AUDIO, 1, small, sizeof(small));
    for (int i = 0; i < 30; i++) add(LINK_AUDIO, (uint8_t)(2 + i), small, sizeof(small));

    LinkDecoder d;
    decode(d, 64);
    TEST_ASSERT_TRUE(seenCount >= 3);
    TEST_ASSERT_EQUAL(LINK_STATE, seen[0].type);
    TEST_ASSERT_EQUAL(100, seen[0].first);
    TEST_ASSERT_EQUAL(1, seen[1].seq);
    TEST_ASSERT_EQUAL(32, d.frames);
    TEST_ASSERT_EQUAL(1, d.crcErrors);
}

/** The sync bytes are allowed in a payload; they are only looked for between frames. */
void test_sync_bytes_inside_a_payload_are_data(void) {
    uint8_t p[16];
    for (size_t i = 0; i < sizeof(p); i++) p[i] = (i & 1) ? LINK_SYNC1 : LINK_SYNC0;
    streamLen = 0;
    add(LINK_AUDIO, 0, p, sizeof(p));
    add(LINK_AUDIO, 1, p, sizeof(p));

    LinkDecoder d;
    decode(d, 1);
    TEST_ASSERT_EQUAL(2, seenCount);
    TEST_ASSERT_EQUAL(LINK_SYNC0, seen[0].first);
    TEST_ASSERT_EQUAL(0, d.skipped);
}

void test_a_length_over_the_maximum_is_noise(void) {
    const uint8_t bogus[] = {LINK_SYNC0, LINK_SYNC1, LINK_AUDIO, 0, 0xFF, 0x7F};
    uint8_t       p[8];
    fill(p, sizeof(p), 0);
    streamLen = 0;
    addBytes(bogus, sizeof(bogus));
    add(LINK_AUDIO, 1, p, sizeof(p));

    LinkDecoder d;
    decode(d, 3);
    TEST_ASSERT_EQUAL(1, seenCount);
    TEST_ASSERT_EQUAL(1, d.badLength);
}

void test_sequence_gaps_are_counted_across_the_wrap(void) {
    uint8_t p[4] = {0};
    streamLen = 0;
    add(LINK_AUDIO, 253, p, sizeof(p));
    add(LINK_AUDIO, 254, p, sizeof(p));
    add(LINK_AUDIO, 1, p, sizeof(p));   // 255 and 0 missing

    LinkDecoder d;
    decode(d, sizeof(stream));
    TEST_ASSERT_EQUAL(3, seenCount);
    TEST_ASSERT_EQUAL(2, d.seqGaps);
}

void test_encode_refuses_what_does_not_fit(void) {
    static uint8_t out[LINK_FRAME_MAX + 8];
    static uint8_t p[LINK_MAX_PAYLOAD + 1];
    TEST_ASSERT_EQUAL(0, linkEncode(out, sizeof(out), LINK_AUDIO, 0, p, LINK_MAX_PAYLOAD + 1));
    TEST_ASSERT_EQUAL(0, linkEncode(out, 10, LINK_AUDIO, 0, p, 10));
    TEST_ASSERT_EQUAL(LINK_FRAME_MAX, linkEncode(out, sizeof(out), LINK_AUDIO, 0, p, LINK_MAX_PAYLOAD));
}

void test_state_round_trips_and_refuses_another_protocol(void) {
    LinkState s;
    s.connected  = true;
    s.streaming  = false;
    s.sampleRate = 44100;
    s.volume     = 38;
    uint8_t b[LINK_STATE_BYTES + 2];
    TEST_ASSERT_EQUAL(LINK_STATE_BYTES, linkPackState(s, b, sizeof(b)));

    LinkState r;
    TEST_ASSERT_TRUE(linkUnpackState(b, LINK_STATE_BYTES, &r));
    TEST_ASSERT_TRUE(r.connected);
    TEST_ASSERT_FALSE(r.streaming);
    TEST_ASSERT_EQUAL(44100, r.sampleRate);
    TEST_ASSERT_EQUAL(38, r.volume);

    TEST_ASSERT_FALSE(linkUnpackState(b, LINK_STATE_BYTES - 1, &r));
    b[0] = LINK_PROTOCOL + 1;
    LinkState untouched;
    TEST_ASSERT_FALSE(linkUnpackState(b, LINK_STATE_BYTES, &untouched));
    TEST_ASSERT_FALSE(untouched.connected);
}

static int runAllTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_is_ccitt_false);
    RUN_TEST(test_one_frame_comes_back_whole);
    RUN_TEST(test_frames_survive_any_split);
    RUN_TEST(test_noise_before_and_between_frames_is_skipped);
    RUN_TEST(test_a_corrupt_frame_is_dropped_and_the_next_one_kept);
    RUN_TEST(test_a_cut_off_frame_does_not_swallow_the_ones_after_it);
    RUN_TEST(test_sync_bytes_inside_a_payload_are_data);
    RUN_TEST(test_a_length_over_the_maximum_is_noise);
    RUN_TEST(test_sequence_gaps_are_counted_across_the_wrap);
    RUN_TEST(test_encode_refuses_what_does_not_fit);
    RUN_TEST(test_state_round_trips_and_refuses_another_protocol);
    return UNITY_END();
}

#ifdef ARDUINO

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
