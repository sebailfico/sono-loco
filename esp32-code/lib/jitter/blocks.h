#ifndef BLOCKS_H
#define BLOCKS_H

/**
 * The encoded blocks a client holds, by sequence number, for rebuilding others.
 *
 * With parity redundancy (D13) a packet carries its own block and the XOR of
 * two older ones, n-1 and n-k. XOR one of those two back out and the other
 * falls out: so a client keeps the blocks it has -- received, or rebuilt --
 * for as far back as a parity can reach, and asks which of a pair it lacks.
 *
 * Pure logic, tested on the host with the rest of lib/jitter. Storage is the
 * caller's, as with JitterBuffer. A slot is chosen by seq modulo SLOTS and
 * remembers the whole seq, so a block from a lap ago is never handed out as
 * the one asked for.
 */

#include <stdint.h>

class BlockStore {
public:
    /** Longer than any distance a server sends (MESH_TX_HISTORY in config.h). */
    static const int SLOTS = 32;

    /** `storage` must hold SLOTS * blockBytes bytes. False if it is null. */
    bool init(uint8_t *storage, int blockBytes);

    void clear();

    /** Keep a copy of block `seq`. */
    void put(uint16_t seq, const uint8_t *block);

    /** Block `seq`, or nullptr if it is not held. */
    const uint8_t *get(uint16_t seq) const;

private:
    uint8_t *buf_   = nullptr;
    int      bytes_ = 0;
    uint16_t seq_[SLOTS]  = {};
    bool     used_[SLOTS] = {};
};

/** out = a XOR b, byte by byte. `out` may be `a` or `b`. */
void xorBlocks(uint8_t *out, const uint8_t *a, const uint8_t *b, int len);

/**
 * A packet numbered `seq` carries the parity of blocks seq-1 and seq-far.
 * If exactly one of them is missing from `store`, that one can be rebuilt:
 * true, its number in *missing, and the block held in *known. Otherwise
 * (both held, or both missing) false.
 */
bool parityRebuildable(const BlockStore &store, uint16_t seq, int far,
                       uint16_t *missing, const uint8_t **known);

#endif  // BLOCKS_H
