#ifndef LINK_H
#define LINK_H

/**
 * The wire between a two-chip server's halves (D18): a front end that runs
 * Bluetooth and nothing else, and the node that runs the mesh. One UART, both
 * ways, carrying frames:
 *
 *   A5 5A  type  seq  len(lo hi)  payload[len]  crc(lo hi)
 *
 * The CRC is CRC-16/CCITT-FALSE over type, seq, len and the payload. A
 * receiver that loses its place -- a byte dropped, the other board booting and
 * printing its ROM banner on the same pins -- drops one byte at a time until
 * it finds a sync whose frame checks, so one bad frame costs that frame and
 * nothing after it.
 *
 * In a library because a framing bug is the kind that shows on a board as
 * nothing at all: a node that never decodes a frame never becomes a server,
 * and says nothing about why. Pure logic, no Arduino or ESP-IDF, tested on the
 * host in test/test_link.
 */

#include <stddef.h>
#include <stdint.h>

#define LINK_SYNC0         0xA5
#define LINK_SYNC1         0x5A
#define LINK_HEADER_BYTES  6
#define LINK_CRC_BYTES     2

/**
 * Longest payload a frame may carry. A receiver buffers one whole frame, and a
 * header claiming more is taken for noise. Audio is sent in chunks that fit
 * (LINK_AUDIO_CHUNK in config.h), not in A2DP's packets, whose size the phone
 * decides.
 */
#define LINK_MAX_PAYLOAD   1024
#define LINK_FRAME_MAX     (LINK_HEADER_BYTES + LINK_MAX_PAYLOAD + LINK_CRC_BYTES)

enum LinkType : uint8_t {
    LINK_AUDIO = 1,   // front end -> node: 16-bit stereo frames, little-endian, after the volume
    LINK_STATE = 2,   // front end -> node: LinkState, on every change and once a second
    LINK_CMD   = 3,   // node -> front end: a command line, run as if typed on its port
    LINK_REPLY = 4,   // front end -> node: what that command printed
};

/** CRC-16/CCITT-FALSE: polynomial 0x1021, from `crc`, no reflection, no final XOR. */
uint16_t linkCrc16(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF);

/**
 * Write one frame into `out`.
 * @return its length, or 0 if the payload is longer than LINK_MAX_PAYLOAD or
 *         the frame does not fit in outSize.
 */
size_t linkEncode(uint8_t *out, size_t outSize, uint8_t type, uint8_t seq,
                  const uint8_t *payload, size_t len);

/** A decoded frame. `payload` points into the decoder, valid until its next push(). */
struct LinkFrame {
    uint8_t        type;
    uint8_t        seq;
    uint16_t       len;
    const uint8_t *payload;
};

/**
 * Turns the bytes off a UART back into frames, in whatever pieces they arrive.
 */
class LinkDecoder {
public:
    /**
     * Feed bytes until a frame completes.
     *
     * @return how many of `data` were taken. If a frame completed, `*frame` is
     *         filled and `*got` set; call again with the rest of `data` --
     *         from data + the return value -- for the next.
     */
    size_t push(const uint8_t *data, size_t len, LinkFrame *frame, bool *got);

    /** Forget any partial frame. The counters are kept. */
    void reset();

    uint32_t frames    = 0;   // decoded
    uint32_t crcErrors = 0;   // a sync and a length that held, and a CRC that did not
    uint32_t badLength = 0;   // a sync followed by a length over LINK_MAX_PAYLOAD
    uint32_t skipped   = 0;   // bytes dropped while looking for a sync
    uint32_t seqGaps   = 0;   // frames missing between two that arrived, by seq

private:
    /** Look at the buffer; true if it begins with a complete frame that checks. */
    bool scan(LinkFrame *frame);
    void drop(size_t n);

    uint8_t buf_[LINK_FRAME_MAX];
    size_t  have_    = 0;
    size_t  consume_ = 0;   // the frame handed out last, removed on the next push
    bool    seqKnown_ = false;
    uint8_t lastSeq_  = 0;
};

/**
 * What the front end says about its phone. Sent whole, never as a change, so
 * a lost frame is corrected by the next one.
 */
#define LINK_PROTOCOL      1   // first byte of a state; a node ignores any other
#define LINK_STATE_BYTES   5

struct LinkState {
    bool     connected  = false;   // a phone is connected
    bool     streaming  = false;   // and sending audio
    uint16_t sampleRate = 0;
    uint8_t  volume     = 0;       // A2DP's 0..127
};

size_t linkPackState(const LinkState &s, uint8_t *out, size_t outSize);
/** False if the payload is short or from another protocol version; `*s` untouched then. */
bool linkUnpackState(const uint8_t *payload, size_t len, LinkState *s);

#endif  // LINK_H
