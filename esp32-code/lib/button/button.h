#ifndef BUTTON_H
#define BUTTON_H

/**
 * The node's one button, read by how long it was held when it is let go.
 *
 * A short press opens Bluetooth (D17), a hold of a few seconds pairs (D12), a
 * long one opens the setup network (D19). Each acts on release, so a hold on
 * its way to a longer action does nothing on the way; the gaps between the
 * ranges are margin for a hand counting seconds, and a hold past the longest
 * range is a way out that does nothing at all.
 *
 * In a library because the edges are the whole of it, and an edge in the
 * wrong place sends a speaker on a wall into the wrong mode with nobody at a
 * console to see why.
 */

#include <stdint.h>

enum ButtonAction : uint8_t {
    BUTTON_NONE = 0,
    BUTTON_BLUETOOTH,   // short press: a phone may connect (D17)
    BUTTON_PAIR,        // a few seconds: offer or join a mesh (D12)
    BUTTON_CONFIG,      // a long hold: the setup network (D19)
};

struct ButtonTimes {
    uint32_t minMs;         // shorter is the contact bouncing
    uint32_t btMaxMs;       // a short press ends before this
    uint32_t pairMinMs;
    uint32_t pairMaxMs;
    uint32_t configMinMs;
    uint32_t configMaxMs;   // longer is a cancel
};

/** What a press held for `heldMs` asks for, on its release. Every range includes its start. */
ButtonAction buttonAction(uint32_t heldMs, const ButtonTimes &t);

/**
 * The action whose range a hold has just entered, between two readings of
 * how long it has been down -- so the node can say "let go now" -- or
 * BUTTON_NONE. The short press has no entry: it is over before anything
 * could be said.
 */
ButtonAction buttonEntered(uint32_t prevMs, uint32_t nowMs, const ButtonTimes &t);

#endif  // BUTTON_H
