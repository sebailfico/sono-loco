#include "button.h"

ButtonAction buttonAction(uint32_t heldMs, const ButtonTimes &t) {
    if (heldMs >= t.minMs && heldMs < t.btMaxMs) return BUTTON_BLUETOOTH;
    if (heldMs >= t.pairMinMs && heldMs < t.pairMaxMs) return BUTTON_PAIR;
    if (heldMs >= t.configMinMs && heldMs < t.configMaxMs) return BUTTON_CONFIG;
    return BUTTON_NONE;
}

ButtonAction buttonEntered(uint32_t prevMs, uint32_t nowMs, const ButtonTimes &t) {
    if (prevMs < t.configMinMs && nowMs >= t.configMinMs && nowMs < t.configMaxMs) return BUTTON_CONFIG;
    if (prevMs < t.pairMinMs && nowMs >= t.pairMinMs && nowMs < t.pairMaxMs) return BUTTON_PAIR;
    return BUTTON_NONE;
}
