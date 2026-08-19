#ifndef JITTER_H
#define JITTER_H

/**
 * Single-producer / single-consumer ring buffer for the CLIENT audio path.
 *
 * Deliberately free of Arduino, FreeRTOS and ESP-IDF dependencies so it can be
 * compiled and tested on the host — see test/test_jitter. This is the code that
 * produced the worst bugs in this project (a permanent 16-bit framing shift),
 * and it is pure logic, so there is no excuse for only ever exercising it on a
 * board.
 *
 * Concurrency contract: `wr` is written only by the producer (the ESP-NOW recv
 * callback), `rd` only by the consumer (loop()). Neither side writes the
 * other's index, so no lock is needed — but that contract is the whole safety
 * argument, so don't add a second writer to either.
 *
 * Storage is supplied by the caller rather than templated on a size, so tests
 * can use a 64-byte buffer and actually reach the wrap-around paths.
 */

#include <stddef.h>
#include <stdint.h>

class JitterBuffer {
public:
    /**
     * `size` MUST be a power of two — the ring masks instead of taking a
     * modulo. Returns false if it is not, or if storage is null.
     */
    bool init(uint8_t *storage, int size);

    /** Discard everything buffered. Producer must not be running. */
    void reset();

    /** Bytes available to read. */
    int fill() const;

    /**
     * Bytes that can still be written. One byte is always left unused so that
     * a full buffer is distinguishable from an empty one.
     */
    int free() const;

    /**
     * Push a whole block or nothing.
     *
     * An earlier version copied byte-by-byte and skipped individual bytes once
     * the ring was full. Dropping an odd number of bytes shifts every later
     * 16-bit sample by one byte — L/R swap, plus each sample assembled from two
     * different sample halves — and it never re-aligns. All-or-nothing keeps
     * the framing intact: a dropped packet is a few ms of glitch, a shifted
     * stream is permanent noise.
     */
    bool pushBlock(const uint8_t *data, int len);

    /** As pushBlock, writing zeroes. Used to keep timing across a packet loss. */
    bool pushSilence(int len);

    /** Copy out without consuming, so the caller advances only what I2S took. */
    bool peek(uint8_t *out, int len) const;

    /** Consume `len` bytes. Caller is responsible for keeping `len` even. */
    void advance(int len);

private:
    uint8_t *buf_  = nullptr;
    int      size_ = 0;
    int      mask_ = 0;

    // volatile: written on one side, read on the other, with no lock between.
    volatile int wr_ = 0;
    volatile int rd_ = 0;
};

#endif  // JITTER_H
