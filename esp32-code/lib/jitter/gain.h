#ifndef GAIN_H
#define GAIN_H

/**
 * A node's own volume trim: a gain in dB applied at its output, after the mesh
 * has its copy, so each room can sit where its amp and speaker need it while
 * the phone's slider still moves every room together.
 *
 * Fixed point, Q12 (4096 = unity): no float in the loop that feeds I2S, and
 * +12 dB of a full-scale sample still fits an int32. Above unity a sample can
 * leave the int16 range; it is clipped, never wrapped -- a wrapped sample is a
 * full-scale spike the other way.
 *
 * Pure logic, tested on the host with the rest of lib/jitter.
 */

#include <stdint.h>

static const int32_t GAIN_UNITY_Q12 = 4096;

/** The Q12 factor for `db` decibels: 10^(db/20) * 4096, rounded. */
int32_t gainQ12FromDb(int db);

/** samples[i] *= q / 4096, clipped to int16, for `count` samples. */
void applyGainQ12(int16_t *samples, int count, int32_t q);

#endif  // GAIN_H
