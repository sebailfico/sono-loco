#ifndef MESH_H
#define MESH_H

/**
 * Mesh identity — which nodes belong to whose household.
 *
 * Every node broadcasts to FF:FF:FF:FF:FF:FF on a fixed channel (D6), so two
 * SonoLoco installations within radio range hear each other perfectly well. A
 * client used to lock onto the first server it heard, which meant the
 * neighbours' stream and yours were decided by whoever powered up first. Every
 * packet now carries a 16-bit mesh id and a client ignores anything that is not
 * its own. See D12.
 *
 * The id is derived from a human-typed mesh name rather than being a number
 * anyone has to remember. That is the whole reason this is a library: it is
 * pure arithmetic on a string, and a mismatch between two nodes is invisible on
 * hardware — a client with the wrong id behaves exactly like a client out of
 * radio range. Silence with no error is not something to debug on a board when
 * it can be pinned by a host test.
 */

#include <stddef.h>
#include <stdint.h>

/**
 * Reserved: "this node has no mesh id of its own yet".
 *
 * Never produced by meshIdFromName() for a non-empty name, so a stored zero
 * unambiguously means unset rather than "hashed to zero". It is not a wildcard:
 * nothing ever matches it, because a wildcard id would silently undo the
 * separation this file exists to provide.
 */
static const uint16_t MESH_ID_UNSET = 0;

/**
 * Longest mesh name kept, excluding the terminator.
 *
 * The name is normalised into a buffer this size *before* it is hashed, so a
 * name longer than this hashes as its truncated form. That is deliberate: the
 * truncated form is what gets stored in NVS and shown in the logs, and a node
 * that hashed the full name would compute an id nothing else in the house could
 * reproduce.
 */
#define MESH_NAME_MAX 31

/**
 * Canonicalise a mesh name: trim the ends, lowercase ASCII, and collapse each
 * run of internal whitespace to a single space.
 *
 * Two people typing "Casa Rossi" and " casa  rossi " mean the same mesh, and
 * the failure mode if they do not is silence with nothing in the log. Written
 * to `out` (always terminated when outSize > 0) and truncated to fit.
 *
 * @return the number of characters written, excluding the terminator.
 */
size_t meshNormaliseName(const char *in, char *out, size_t outSize);

/**
 * The mesh id a name maps to: FNV-1a over the normalised name, folded to 16
 * bits.
 *
 * A name and a hash rather than a number the user types, because the number is
 * not the point — being able to say "our mesh is called Casa Rossi" is. The
 * cost is a 1-in-65536 chance that two neighbouring meshes collide, which is
 * why pairing (which copies an id rather than guessing at a name) exists.
 *
 * @return MESH_ID_UNSET for an empty or whitespace-only name; never
 *         MESH_ID_UNSET for any other input.
 */
uint16_t meshIdFromName(const char *name);

/**
 * The sequence number that marks a packet as a pairing beacon rather than
 * audio.
 *
 * A beacon is "this mesh is open to being joined, right now" and carries no
 * payload: the mesh id in the header is the entire message. It reuses the audio
 * packet layout rather than adding a type byte, so the wire format did not have
 * to change twice -- a beacon is simply a packet with no payload, and every
 * receiver already drops those before they reach the jitter buffer.
 *
 * The value only has to be unlikely to be confused with a real sequence number
 * on a zero-length packet, and a zero-length audio packet is never sent at all.
 */
static const uint16_t MESH_BEACON_SEQ = 0xBEAC;

/**
 * Is this a pairing beacon?
 *
 * Both halves matter. Length alone would make any truncated or corrupt frame a
 * beacon, and the sequence number alone would make one audio packet in every
 * 65536 one -- at 220 packets/s that is a false beacon every five minutes, in a
 * path whose whole job is to not join the wrong mesh.
 */
bool meshIsBeacon(uint16_t seq, uint16_t len);

/**
 * The sequence number that marks an update request: "the node named in the
 * payload, reboot into update mode" (see the update mode section of main.cpp).
 *
 * Unlike a beacon it does not travel in the audio group. It is stamped with a
 * group of its own, so a node from before update mode reads it as another
 * mesh's traffic and drops it -- rather than as audio from a new sender, which
 * it would lock onto before ever looking at the length, and then ignore its
 * real server. The sequence number still says which command this is.
 */
static const uint16_t MESH_UPDATE_SEQ = 0x0DA7;

/**
 * Longest update target, excluding the terminator. A target is a ROOM_NAME or
 * a MAC address written AA:BB:CC:DD:EE:FF, and it is the whole payload.
 */
#define MESH_TARGET_MAX 31

/** Is this an update request? The magic, and a target of 1..MESH_TARGET_MAX bytes. */
bool meshIsUpdateRequest(uint16_t seq, uint16_t len);

/**
 * Does an update request naming `target` mean this node?
 *
 * True when the target is this node's room name or its MAC, ignoring case --
 * "sonoloco-c3" is what a hostname looks like and "SonoLoco-C3" what the build
 * says, and they are the same node. Whole strings only: "SonoLoco-WROVER" must
 * not reboot "SonoLoco-WROVER2", which a prefix match would.
 *
 * `target` comes off the air, so it is `len` bytes with no terminator. There is
 * deliberately no wildcard: rebooting every speaker in the house off the mesh
 * at once is not something to be one character away from. A command may be
 * addressed to every node (meshCommandAddressed), but only one that cannot
 * reboot a node or take it off the mesh.
 */
bool meshTargetMatches(const char *target, size_t len, const char *roomName, const char *mac);

// ---------------------------------------------------------------------------
// Commands over the mesh (D16)
// ---------------------------------------------------------------------------
//
// Any serial command can run on another node of the same mesh: `@<target>
// <command>` typed on a node on USB, e.g. `@SonoLoco-Stereo v-20`, or `@* N`
// for every node. The node named runs it exactly as if it had been typed on
// its own port, and what it printed comes back as replies, printed by the
// asking node as `[@<name>] <line>`.
//
// Both travel in the control group, like an update request. A request:
//
//   id (2, little-endian) | target length (1) | target | command text
//
// and a reply, one or more per request and node:
//
//   id (2, little-endian) | part (1) | name length (1) | name | text
//
// `id` is the asking node's, so it can tell its own replies from another
// node's conversation. `part` counts from 0; MESH_REPLY_CONT marks a part whose
// text continues the previous part's last line, which happens only to a line
// longer than one packet holds.

/** The sequence numbers that mark a command and a reply in the control group. */
static const uint16_t MESH_COMMAND_SEQ = 0xC0DE;
static const uint16_t MESH_REPLY_SEQ   = 0xA45E;

/** Longest command text, excluding the terminator: a letter and its argument. */
#define MESH_COMMAND_MAX 63

/** Every node of the mesh, as a target. Commands only, see meshCommandAddressed. */
#define MESH_TARGET_ALL "*"

/** In a reply's `part` byte: this part continues the previous part's last line. */
#define MESH_REPLY_CONT 0x80

/** Bytes before a reply's text: id, part, name length, and the name. */
#define MESH_REPLY_HEADER(nameLen) (4 + (nameLen))

/** A request off the air. The pointers point into the payload; nothing is terminated. */
struct MeshCommand {
    uint16_t    id;
    const char *target;
    size_t      targetLen;
    const char *text;
    size_t      textLen;
};

/** A reply off the air, likewise. */
struct MeshReply {
    uint16_t    id;
    uint8_t     part;   // with MESH_REPLY_CONT
    const char *name;
    size_t      nameLen;
    const char *text;
    size_t      textLen;
};

/**
 * Split what follows `@` into a target and a command: `SonoLoco-C3 v-20` is
 * the target `SonoLoco-C3` and the command `v-20`. Spaces around either are
 * dropped; the command keeps those inside it (an argument may contain them).
 * Nothing is copied: both are pointers into `line`, with their lengths.
 *
 * @return false unless there is a target of 1..MESH_TARGET_MAX characters and
 *         a command of 1..MESH_COMMAND_MAX.
 */
bool meshSplitCommandLine(const char *line, const char **target, size_t *targetLen,
                          const char **text, size_t *textLen);

/** @return the payload's length, or 0 if it does not fit `cap` or is malformed. */
size_t meshPackCommand(uint8_t *out, size_t cap, uint16_t id, const char *target,
                       size_t targetLen, const char *text, size_t textLen);

/** @return false for anything that is not a well-formed request. */
bool meshUnpackCommand(const uint8_t *in, size_t len, MeshCommand *out);

/** @return the payload's length, or 0 if the header alone does not fit `cap`. */
size_t meshPackReply(uint8_t *out, size_t cap, uint16_t id, uint8_t part, const char *name,
                     const char *text, size_t textLen);

/** @return false for anything that is not a well-formed reply. */
bool meshUnpackReply(const uint8_t *in, size_t len, MeshReply *out);

/**
 * Which part of whose answer a reply is, as one number: a node answers again
 * for every later copy of a request it hears, and the asking node prints each
 * part once. The part's continuation bit is part of it.
 */
uint32_t meshReplyKey(const char *name, size_t nameLen, uint8_t part);

/**
 * How much of `text` goes into the next reply part, given room for `cap`
 * bytes: all of it if it fits, otherwise up to and including the last newline
 * that fits, and only a line longer than a whole part is cut mid-line.
 */
size_t meshReplyChunk(const char *text, size_t len, size_t cap);

/**
 * May this command travel the mesh at all? Not `W`, which carries the home
 * network's password and would broadcast it in the clear, and not `@` itself:
 * a node does not relay for another.
 */
bool meshCommandAllowedRemote(char cmd);

/**
 * May this command be sent to every node at once? Not one that reboots a node
 * (U b n c), moves it to another mesh or opens pairing (g p o), or stops its
 * radio (w): each of those, sent to `*`, takes the whole house off the air,
 * and only a node named by the person who meant it should do that.
 */
bool meshCommandAllowedForAll(char cmd);

/**
 * Is a command with this target, beginning with `cmd`, for this node? Its room
 * name or MAC as for an update request, or `*` for a command allowed for all.
 */
bool meshCommandAddressed(const char *target, size_t len, char cmd, const char *roomName,
                          const char *mac);

#endif  // MESH_H
