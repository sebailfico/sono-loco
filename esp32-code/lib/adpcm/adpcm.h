#ifndef ADPCM_H
#define ADPCM_H

/**
 * IMA ADPCM, stereo, in self-contained blocks: the mesh's audio codec.
 *
 * Why a codec at all, and why this one (D5): the mesh used to carry 22.05 kHz
 * mono PCM, because 44.1 kHz stereo PCM is four times the bytes and the radio
 * cannot afford it next to Bluetooth. IMA ADPCM stores each sample as a 4-bit
 * step from a prediction, so 44.1 kHz stereo costs what 22.05 kHz mono PCM
 * did. An A/B on 2026-09-29 (tools/codec/abtest.py) put it at 28-31 dB SNR on
 * music, with the treble and the stereo image intact; the listener preferred
 * it "way better" to the old path.
 *
 * Integer only, a handful of operations per sample: cheap on every chip here,
 * including a C3 with no FPU.
 *
 * A block carries the decoder's state at its first frame, so any block decodes
 * on its own. That is what lets a packet carry the previous block as well as
 * its own, and a client rebuild a lost packet from the next one.
 *
 * Block layout, `adpcmBlockBytes(frames)` bytes:
 *
 *   [0..1] left predictor, int16 little-endian   [2] left step index
 *   [3..4] right predictor, int16 little-endian  [5] right step index
 *   [6..]  one byte per stereo frame: low nibble left, high nibble right
 *
 * The reference implementation is tools/codec/abtest.py; test/test_adpcm pins
 * this one to its output.
 */

#include <stddef.h>
#include <stdint.h>

struct AdpcmState {
    int16_t pred  = 0;
    uint8_t index = 0;
};

/** Encode one sample; returns the 4-bit code and advances the state. */
uint8_t adpcmEncodeSample(AdpcmState &s, int16_t x);

/** Decode one 4-bit code; returns the sample and advances the state. */
int16_t adpcmDecodeSample(AdpcmState &s, uint8_t code);

static const int ADPCM_BLOCK_HEADER = 6;
/** Frames one block may hold. Bounds the encoder's buffer, nothing else. */
static const int ADPCM_MAX_FRAMES   = 240;

constexpr size_t adpcmBlockBytes(int frames) {
    return (size_t)ADPCM_BLOCK_HEADER + (size_t)frames;
}

/**
 * Turns a stream of stereo frames into blocks of a fixed number of frames.
 * The state runs on from block to block; each block records where it started.
 */
class AdpcmStereoEncoder {
public:
    /** False if `framesPerBlock` is out of range; the encoder is then unusable. */
    bool begin(int framesPerBlock);

    /** Forget the stream: the next frame starts a block from a zero state. */
    void reset();

    /**
     * Add one frame. Returns true when it completed a block, which is then in
     * block() until the next push() -- copy it out before pushing again.
     */
    bool push(int16_t left, int16_t right);

    const uint8_t *block() const { return buf_; }
    size_t blockBytes() const { return adpcmBlockBytes(frames_); }

private:
    AdpcmState st_[2];
    uint8_t    buf_[ADPCM_BLOCK_HEADER + ADPCM_MAX_FRAMES];
    int        frames_ = 0;
    int        filled_ = 0;
};

/**
 * Decode one block of `frames` frames into interleaved stereo (L, R, L, R...),
 * `2 * frames` samples. Stateless: everything it needs is in the block.
 * Returns false, writing nothing, for an index out of range -- a block that
 * cannot have come from an encoder.
 */
bool adpcmDecodeStereoBlock(const uint8_t *block, int frames, int16_t *out);

#endif  // ADPCM_H
