#ifndef HOLES_H
#define HOLES_H

/**
 * Where each recently lost block's silence went in the jitter buffer.
 *
 * A packet carries its own block and the one from `distance` packets before
 * (D13). When a packet is missed, its block is pushed as silence -- playback
 * keeps its timing -- and the silence's position is recorded here. When the
 * later packet carrying that block arrives, the block is written over the
 * silence (JitterBuffer::patch), provided it has not played yet.
 *
 * Pure logic, tested on the host with the ring (test/test_jitter). A slot is
 * chosen by seq modulo SLOTS and remembers the whole seq, so an entry from a
 * lap ago is never taken for the one wanted now.
 */

#include <stdint.h>

class HoleTable {
public:
    /** Longer than any distance a server sends (MESH_TX_HISTORY in config.h). */
    static const int SLOTS = 32;

    void clear();

    /** `seq` was pushed as silence, starting at ring position `pos`. */
    void add(uint16_t seq, int pos);

    /** If `seq` is a recorded hole: true, its position in *pos, and it is forgotten. */
    bool take(uint16_t seq, int *pos);

private:
    struct Slot {
        uint16_t seq;
        int      pos;
        bool     used;
    };
    Slot slots_[SLOTS] = {};
};

#endif  // HOLES_H
