/**
 * Tests for the mesh codec (lib/adpcm).
 *
 * Run with:  pio test -e native      (or -e esp32dev on a board)
 *
 * The golden vectors come from tools/codec/abtest.py, the reference the A/B
 * listening test was made with. If they ever disagree, the firmware is no
 * longer the codec that was listened to -- and two nodes built on either side
 * of such a change would decode each other's blocks into noise.
 */

#include <unity.h>

#include <string.h>

#include "adpcm.h"

void setUp(void) {}
void tearDown(void) {}

// A loud sine, a step to +full scale, a step to -full scale: growth of the step
// index, clamping at both rails, and recovery.
static const int16_t GOLDEN_IN[48] = {
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087,
    -12000, -11087, -8485, -4592, 0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
    -32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768,
    -32768, -32768};
static const uint8_t GOLDEN_CODE[48] = {
    0, 7, 7, 7, 7, 7, 7, 7, 14, 14, 11, 10, 8, 0, 3, 5, 4, 3, 2, 2, 0, 8, 11, 12,
    7, 7, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 15, 15, 11, 8, 0, 8, 8, 0, 8, 8, 0, 8};
static const int16_t GOLDEN_OUT[48] = {
    0, 11, 41, 104, 240, 533, 1164, 2521, -1, -4467, -8727, -11494, -11997, -11540,
    -8631, -4473, 508, 5195, 8238, 11005, 11508, 11051, 8142, 4740, 11602, 26310,
    32616, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 20610,
    -5449, -31518, -32768, -29691, -32489, -32768, -30456, -32558, -32768, -31031,
    -32610};

static void test_encoder_matches_the_reference(void) {
    AdpcmState s;
    for (int i = 0; i < 48; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(GOLDEN_CODE[i], adpcmEncodeSample(s, GOLDEN_IN[i]),
                                        "code differs from abtest.py");
        TEST_ASSERT_EQUAL_INT16(GOLDEN_OUT[i], s.pred);
    }
}

static void test_decoder_reconstructs_what_the_encoder_tracked(void) {
    AdpcmState s;
    for (int i = 0; i < 48; i++) {
        TEST_ASSERT_EQUAL_INT16(GOLDEN_OUT[i], adpcmDecodeSample(s, GOLDEN_CODE[i]));
    }
}

// Two blocks of four frames, byte for byte as abtest.py lays them out. This is
// the wire format: a change here is a change every node must take at once.
static void test_block_layout_matches_the_reference(void) {
    static const int16_t L[8] = {1000, 2000, -3000, 500, 0, 7000, -7000, 32767};
    static const int16_t R[8] = {-5, 5, 100, -100, 30000, -30000, 0, 1};
    static const uint8_t B0[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB7, 0x57, 0x7F, 0xF7};
    static const uint8_t B1[10] = {0x72, 0x00, 0x20, 0xEE, 0xFF, 0x14, 0x7A, 0xF7, 0x2F, 0x87};

    AdpcmStereoEncoder enc;
    TEST_ASSERT_TRUE(enc.begin(4));
    TEST_ASSERT_EQUAL_UINT32(10, enc.blockBytes());
    for (int i = 0; i < 3; i++) TEST_ASSERT_FALSE(enc.push(L[i], R[i]));
    TEST_ASSERT_TRUE(enc.push(L[3], R[3]));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(B0, enc.block(), 10);
    for (int i = 4; i < 7; i++) TEST_ASSERT_FALSE(enc.push(L[i], R[i]));
    TEST_ASSERT_TRUE(enc.push(L[7], R[7]));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(B1, enc.block(), 10);
}

// The property the redundancy depends on: the second block decodes on its own,
// from nothing but its own bytes, to exactly what a decoder that had heard the
// whole stream would have produced.
static void test_a_block_decodes_without_its_predecessor(void) {
    static const int16_t RL[8] = {11, 41, -22, 114, 17, 283, -291, 942};
    static const int16_t RR[8] = {-4, 4, 23, -18, 75, -124, 19, -7};
    static const uint8_t B1[10] = {0x72, 0x00, 0x20, 0xEE, 0xFF, 0x14, 0x7A, 0xF7, 0x2F, 0x87};
    int16_t out[8];
    TEST_ASSERT_TRUE(adpcmDecodeStereoBlock(B1, 4, out));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT16(RL[4 + i], out[2 * i]);
        TEST_ASSERT_EQUAL_INT16(RR[4 + i], out[2 * i + 1]);
    }
}

// Encode a long stereo stream in blocks, decode every block independently, and
// the result must be sample for sample what the encoder reconstructed.
static void test_blockwise_decode_equals_continuous(void) {
    const int F = 114, BLOCKS = 6;
    AdpcmStereoEncoder enc;
    TEST_ASSERT_TRUE(enc.begin(F));
    AdpcmState tl, tr;   // shadow encoders, for the continuous reconstruction
    int16_t out[2 * 114];
    uint32_t seed = 12345;
    for (int b = 0; b < BLOCKS; b++) {
        int16_t expL[114], expR[114];
        bool done = false;
        for (int i = 0; i < F; i++) {
            seed = seed * 1103515245u + 12345u;
            // Loud, and different on each channel, so a swapped nibble shows.
            const int16_t l = (int16_t)((int32_t)(seed >> 8) % 20000);
            const int16_t r = (int16_t)(-l / 3 + (int32_t)(seed & 0xFF) * 16);
            adpcmEncodeSample(tl, l); expL[i] = tl.pred;
            adpcmEncodeSample(tr, r); expR[i] = tr.pred;
            done = enc.push(l, r);
        }
        TEST_ASSERT_TRUE(done);
        TEST_ASSERT_TRUE(adpcmDecodeStereoBlock(enc.block(), F, out));
        for (int i = 0; i < F; i++) {
            TEST_ASSERT_EQUAL_INT16(expL[i], out[2 * i]);
            TEST_ASSERT_EQUAL_INT16(expR[i], out[2 * i + 1]);
        }
    }
}

static void test_silence_stays_silent(void) {
    AdpcmState s;
    for (int i = 0; i < 1000; i++) adpcmEncodeSample(s, 0);
    TEST_ASSERT_EQUAL_INT16(0, s.pred);
    TEST_ASSERT_EQUAL_UINT8(0, s.index);
}

static void test_a_corrupt_index_is_refused(void) {
    uint8_t block[6 + 4] = {0, 0, 89, 0, 0, 0, 0, 0, 0, 0};   // left index 89: impossible
    int16_t out[8];
    memset(out, 0x55, sizeof(out));
    TEST_ASSERT_FALSE(adpcmDecodeStereoBlock(block, 4, out));
    TEST_ASSERT_EQUAL_INT16(0x5555, out[0]);   // nothing written
}

static void test_block_size_is_bounded(void) {
    AdpcmStereoEncoder enc;
    TEST_ASSERT_FALSE(enc.begin(0));
    TEST_ASSERT_FALSE(enc.begin(ADPCM_MAX_FRAMES + 1));
    TEST_ASSERT_FALSE(enc.push(1, 1));   // unusable after a refused begin()
    TEST_ASSERT_TRUE(enc.begin(ADPCM_MAX_FRAMES));
}

static int runAllTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_encoder_matches_the_reference);
    RUN_TEST(test_decoder_reconstructs_what_the_encoder_tracked);
    RUN_TEST(test_block_layout_matches_the_reference);
    RUN_TEST(test_a_block_decodes_without_its_predecessor);
    RUN_TEST(test_blockwise_decode_equals_continuous);
    RUN_TEST(test_silence_stays_silent);
    RUN_TEST(test_a_corrupt_index_is_refused);
    RUN_TEST(test_block_size_is_bounded);
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
