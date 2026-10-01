#include "gain.h"

#include <math.h>

int32_t gainQ12FromDb(int db) {
    return (int32_t)lround(pow(10.0, db / 20.0) * GAIN_UNITY_Q12);
}

void applyGainQ12(int16_t *samples, int count, int32_t q) {
    if (q == GAIN_UNITY_Q12) return;
    for (int i = 0; i < count; i++) {
        // Rounded, so a small gain does not bias every sample towards -inf.
        int32_t v = (samples[i] * q + (GAIN_UNITY_Q12 / 2)) >> 12;
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        samples[i] = (int16_t)v;
    }
}
