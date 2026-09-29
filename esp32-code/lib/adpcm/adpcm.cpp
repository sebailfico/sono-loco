#include "adpcm.h"

// The standard IMA/DVI tables.
static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// The decoder's update, shared by both directions: the encoder tracks exactly
// what the decoder will reconstruct, so the two cannot drift apart.
static inline int16_t step(AdpcmState &s, uint8_t code) {
    const int st = STEP[s.index];
    int diff = st >> 3;
    if (code & 4) diff += st;
    if (code & 2) diff += st >> 1;
    if (code & 1) diff += st >> 2;
    int pred = (code & 8) ? s.pred - diff : s.pred + diff;
    if (pred > 32767)  pred = 32767;
    if (pred < -32768) pred = -32768;
    int idx = s.index + INDEX[code & 7];
    if (idx < 0)  idx = 0;
    if (idx > 88) idx = 88;
    s.pred  = (int16_t)pred;
    s.index = (uint8_t)idx;
    return s.pred;
}

uint8_t adpcmEncodeSample(AdpcmState &s, int16_t x) {
    const int st = STEP[s.index];
    int diff = (int)x - s.pred;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= st)      { code |= 4; diff -= st; }
    if (diff >= st >> 1) { code |= 2; diff -= st >> 1; }
    if (diff >= st >> 2) { code |= 1; }
    step(s, code);
    return code;
}

int16_t adpcmDecodeSample(AdpcmState &s, uint8_t code) {
    return step(s, code & 0x0F);
}

static inline void putState(uint8_t *p, const AdpcmState &s) {
    p[0] = (uint8_t)(s.pred & 0xFF);
    p[1] = (uint8_t)((uint16_t)s.pred >> 8);
    p[2] = s.index;
}

static inline void getState(const uint8_t *p, AdpcmState &s) {
    s.pred  = (int16_t)(uint16_t)(p[0] | (p[1] << 8));
    s.index = p[2];
}

bool AdpcmStereoEncoder::begin(int framesPerBlock) {
    if (framesPerBlock < 1 || framesPerBlock > ADPCM_MAX_FRAMES) {
        frames_ = 0;
        return false;
    }
    frames_ = framesPerBlock;
    reset();
    return true;
}

void AdpcmStereoEncoder::reset() {
    st_[0] = AdpcmState();
    st_[1] = AdpcmState();
    filled_ = 0;
}

bool AdpcmStereoEncoder::push(int16_t left, int16_t right) {
    if (frames_ == 0) return false;
    if (filled_ == 0) {
        // Where this block starts is what the decoder needs to start there too.
        putState(buf_, st_[0]);
        putState(buf_ + 3, st_[1]);
    }
    const uint8_t l = adpcmEncodeSample(st_[0], left);
    const uint8_t r = adpcmEncodeSample(st_[1], right);
    buf_[ADPCM_BLOCK_HEADER + filled_] = (uint8_t)(l | (r << 4));
    if (++filled_ < frames_) return false;
    filled_ = 0;
    return true;
}

bool adpcmDecodeStereoBlock(const uint8_t *block, int frames, int16_t *out) {
    AdpcmState l, r;
    getState(block, l);
    getState(block + 3, r);
    if (l.index > 88 || r.index > 88) return false;
    const uint8_t *codes = block + ADPCM_BLOCK_HEADER;
    for (int i = 0; i < frames; i++) {
        out[2 * i]     = step(l, codes[i] & 0x0F);
        out[2 * i + 1] = step(r, codes[i] >> 4);
    }
    return true;
}
