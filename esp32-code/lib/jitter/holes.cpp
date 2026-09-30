#include "holes.h"

void HoleTable::clear() {
    for (auto &s : slots_) s.used = false;
}

void HoleTable::add(uint16_t seq, int pos) {
    Slot &s = slots_[seq % SLOTS];
    s.seq  = seq;
    s.pos  = pos;
    s.used = true;
}

bool HoleTable::take(uint16_t seq, int *pos) {
    Slot &s = slots_[seq % SLOTS];
    if (!s.used || s.seq != seq) return false;
    s.used = false;
    *pos   = s.pos;
    return true;
}
