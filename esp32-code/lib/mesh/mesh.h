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

#endif  // MESH_H
