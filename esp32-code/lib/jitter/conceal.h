#ifndef CONCEAL_H
#define CONCEAL_H

/**
 * What a lost block plays instead of silence.
 *
 * A block that never arrived and was not rebuilt (D13) used to be zeroes: the
 * waveform drops from wherever it was to 0 and jumps back 2.6 ms later, and a
 * step is a click whatever the music. What is needed is not a good guess at
 * the missing audio but no step at either edge.
 *
 * Both neighbours are known when the hole is made: a gap is noticed when the
 * packet after it arrives, so the block after the hole is in hand along with
 * the one before it. The hole is filled with each neighbour played backwards
 * away from the edge it shares -- `prev` from its last frame, `next` into its
 * first -- so the first frame of the fill is the last frame of `prev` and the
 * last frame of the fill is the first frame of `next`. A mirror image is
 * continuous where it meets the original, and it has the same spectrum, so
 * the hole sounds like more of the same rather than like a gap.
 *
 *   run of 1:  prev mirrored, crossfaded across the block into next mirrored
 *   run of k:  the first block is prev mirrored fading out, the last is next
 *              mirrored fading in, and anything between is silence
 *
 * A neighbour that is not known (the stream's first block, an undecodable
 * one) is passed as nullptr and contributes nothing: its side fades from or
 * to zero, which is still no step.
 *
 * Each block of a run is made on its own, so a block patched later by a late
 * copy (JitterBuffer::patch) replaces exactly its own fill and nothing beside
 * it is touched twice.
 *
 * Pure logic, tested on the host with the rest of lib/jitter. Stereo frames,
 * interleaved int16 L/R.
 */

#include <stdint.h>

/**
 * Fill block `index` (0-based) of a run of `run` lost blocks of `frames`
 * stereo frames each, into `out` (2 * frames samples). `prev` is the block
 * played just before the run, `next` the one just after; either may be
 * nullptr. `out` must not alias either.
 */
void concealBlock(const int16_t *prev, const int16_t *next, int frames,
                  int run, int index, int16_t *out);

#endif  // CONCEAL_H
