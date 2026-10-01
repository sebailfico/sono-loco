#include "conceal.h"

void concealBlock(const int16_t *prev, const int16_t *next, int frames,
                  int run, int index, int16_t *out) {
    // Which neighbour this block can see: the first block of a run touches
    // prev, the last touches next, a run of one touches both.
    const int16_t *from = (index == 0)       ? prev : nullptr;
    const int16_t *to   = (index == run - 1) ? next : nullptr;
    const int      span = frames > 1 ? frames - 1 : 1;

    for (int i = 0; i < frames; i++) {
        // Integer weights, 0..span: no float in the receive callback, and the
        // two ends exact -- frame 0 is all prev, frame frames-1 all next.
        const int32_t wTo   = i;
        const int32_t wFrom = span - i;
        for (int c = 0; c < 2; c++) {
            int32_t acc = 0;
            if (from) acc += wFrom * from[2 * (frames - 1 - i) + c];   // prev, backwards from its end
            if (to)   acc += wTo   * to[2 * (frames - 1 - i) + c];     // next, backwards into its start
            out[2 * i + c] = (int16_t)(acc / span);
        }
    }
}
