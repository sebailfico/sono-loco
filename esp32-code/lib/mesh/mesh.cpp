#include "mesh.h"

namespace {

inline bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

inline char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

}  // namespace

size_t meshNormaliseName(const char *in, char *out, size_t outSize) {
    if (outSize == 0) return 0;
    out[0] = '\0';
    if (in == nullptr) return 0;

    size_t n = 0;
    bool pendingSpace = false;   // a space is only emitted once something follows it

    for (const char *p = in; *p != '\0'; p++) {
        if (isSpace(*p)) {
            if (n > 0) pendingSpace = true;   // leading whitespace is dropped outright
            continue;
        }
        if (pendingSpace) {
            if (n + 1 >= outSize) break;
            out[n++] = ' ';
            pendingSpace = false;
        }
        if (n + 1 >= outSize) break;
        out[n++] = lower(*p);
    }
    // pendingSpace left set here is trailing whitespace, and is simply dropped.

    out[n] = '\0';
    return n;
}

uint16_t meshIdFromName(const char *name) {
    char norm[MESH_NAME_MAX + 1];
    const size_t n = meshNormaliseName(name, norm, sizeof(norm));
    if (n == 0) return MESH_ID_UNSET;

    // FNV-1a, 32-bit. Chosen for being four lines and having no state to get
    // wrong; this hashes a handful of characters once per boot, so nothing about
    // its speed or its distribution is load-bearing beyond "different names
    // usually differ".
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)norm[i];
        h *= 16777619u;
    }

    // Fold both halves in rather than truncating, so a change anywhere in the
    // name can reach every bit of the result.
    uint16_t id = (uint16_t)((h >> 16) ^ (h & 0xFFFFu));

    // Zero is reserved for "unset". Displacing the one name in 65536 that lands
    // there costs nothing; letting a real mesh call itself unset would make that
    // household's nodes ignore each other forever.
    if (id == MESH_ID_UNSET) id = 1;
    return id;
}

bool meshIsBeacon(uint16_t seq, uint16_t len) {
    return len == 0 && seq == MESH_BEACON_SEQ;
}
