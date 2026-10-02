#include "mesh.h"

#include <string.h>

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

bool meshIsUpdateRequest(uint16_t seq, uint16_t len) {
    return seq == MESH_UPDATE_SEQ && len >= 1 && len <= MESH_TARGET_MAX;
}

namespace {

/** `len` bytes of `a` against the whole of the terminated `b`, ignoring ASCII case. */
bool equalsIgnoreCase(const char *a, size_t len, const char *b) {
    if (a == nullptr || b == nullptr) return false;
    for (size_t i = 0; i < len; i++) {
        if (b[i] == '\0' || lower(a[i]) != lower(b[i])) return false;
    }
    return b[len] == '\0';
}

}  // namespace

bool meshTargetMatches(const char *target, size_t len, const char *roomName, const char *mac) {
    if (len == 0 || len > MESH_TARGET_MAX) return false;
    return equalsIgnoreCase(target, len, roomName) || equalsIgnoreCase(target, len, mac);
}

bool meshSplitCommandLine(const char *line, const char **target, size_t *targetLen,
                          const char **text, size_t *textLen) {
    if (line == nullptr) return false;
    while (*line == ' ') line++;
    const char *t = line;
    while (*line != '\0' && *line != ' ') line++;
    const size_t tlen = (size_t)(line - t);
    while (*line == ' ') line++;

    size_t clen = strlen(line);
    while (clen > 0 && isSpace(line[clen - 1])) clen--;

    if (tlen == 0 || tlen > MESH_TARGET_MAX || clen == 0 || clen > MESH_COMMAND_MAX) return false;
    *target    = t;
    *targetLen = tlen;
    *text      = line;
    *textLen   = clen;
    return true;
}

size_t meshPackCommand(uint8_t *out, size_t cap, uint16_t id, const char *target,
                       size_t targetLen, const char *text, size_t textLen) {
    if (target == nullptr || text == nullptr) return 0;
    const size_t clen = textLen;
    if (targetLen == 0 || targetLen > MESH_TARGET_MAX || clen == 0 || clen > MESH_COMMAND_MAX) return 0;
    const size_t n = 3 + targetLen + clen;
    if (n > cap) return 0;
    out[0] = (uint8_t)(id & 0xFF);
    out[1] = (uint8_t)(id >> 8);
    out[2] = (uint8_t)targetLen;
    memcpy(out + 3, target, targetLen);
    memcpy(out + 3 + targetLen, text, clen);
    return n;
}

bool meshUnpackCommand(const uint8_t *in, size_t len, MeshCommand *out) {
    if (in == nullptr || len < 3) return false;
    const size_t tlen = in[2];
    if (tlen == 0 || tlen > MESH_TARGET_MAX || 3 + tlen >= len) return false;
    const size_t clen = len - 3 - tlen;
    if (clen > MESH_COMMAND_MAX) return false;
    out->id        = (uint16_t)(in[0] | (in[1] << 8));
    out->target    = (const char *)in + 3;
    out->targetLen = tlen;
    out->text      = (const char *)in + 3 + tlen;
    out->textLen   = clen;
    return true;
}

size_t meshPackReply(uint8_t *out, size_t cap, uint16_t id, uint8_t part, const char *name,
                     const char *text, size_t textLen) {
    if (name == nullptr) return 0;
    const size_t nlen = strlen(name);
    if (nlen == 0 || nlen > MESH_TARGET_MAX || MESH_REPLY_HEADER(nlen) + textLen > cap) return 0;
    out[0] = (uint8_t)(id & 0xFF);
    out[1] = (uint8_t)(id >> 8);
    out[2] = part;
    out[3] = (uint8_t)nlen;
    memcpy(out + 4, name, nlen);
    if (textLen) memcpy(out + MESH_REPLY_HEADER(nlen), text, textLen);
    return MESH_REPLY_HEADER(nlen) + textLen;
}

bool meshUnpackReply(const uint8_t *in, size_t len, MeshReply *out) {
    if (in == nullptr || len < 4) return false;
    const size_t nlen = in[3];
    if (nlen == 0 || nlen > MESH_TARGET_MAX || MESH_REPLY_HEADER(nlen) > len) return false;
    out->id      = (uint16_t)(in[0] | (in[1] << 8));
    out->part    = in[2];
    out->name    = (const char *)in + 4;
    out->nameLen = nlen;
    out->text    = (const char *)in + MESH_REPLY_HEADER(nlen);
    out->textLen = len - MESH_REPLY_HEADER(nlen);
    return true;
}

uint32_t meshReplyKey(const char *name, size_t nameLen, uint8_t part) {
    // FNV-1a over the name, ignoring case as targets do, then the part.
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < nameLen; i++) {
        h ^= (uint8_t)lower(name[i]);
        h *= 16777619u;
    }
    h ^= part;
    h *= 16777619u;
    return h;
}

size_t meshReplyChunk(const char *text, size_t len, size_t cap) {
    if (len <= cap) return len;
    for (size_t i = cap; i > 0; i--) {
        if (text[i - 1] == '\n') return i;
    }
    return cap;
}

bool meshCommandAllowedRemote(char cmd) {
    return cmd != '\0' && cmd != 'W' && cmd != '@';
}

bool meshCommandAllowedForAll(char cmd) {
    return meshCommandAllowedRemote(cmd) && strchr("UbncgpowW@", cmd) == nullptr;
}

bool meshCommandAddressed(const char *target, size_t len, char cmd, const char *roomName,
                          const char *mac) {
    if (!meshCommandAllowedRemote(cmd)) return false;
    if (len == 1 && target != nullptr && target[0] == MESH_TARGET_ALL[0]) {
        return meshCommandAllowedForAll(cmd);
    }
    return meshTargetMatches(target, len, roomName, mac);
}
