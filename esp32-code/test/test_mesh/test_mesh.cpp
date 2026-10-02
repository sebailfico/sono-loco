/**
 * Host-side tests for mesh identity.
 *
 * Run with:  pio test -e native      (or -e esp32dev / -e esp32c3 on a board)
 *
 * These matter more than their size suggests. A node whose mesh id does not
 * match the source's is indistinguishable, from the outside, from a node out of
 * radio range: no audio, no error, nothing in the log. Every way two nodes could
 * disagree about what "Casa Rossi" hashes to is therefore pinned here, on the
 * host, rather than discovered on a bench with three boards and a stopwatch.
 */

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "mesh.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Name normalisation
// ---------------------------------------------------------------------------

static const char *norm(const char *in) {
    static char out[MESH_NAME_MAX + 1];
    meshNormaliseName(in, out, sizeof(out));
    return out;
}

void test_normalise_trims_lowercases_and_collapses(void) {
    TEST_ASSERT_EQUAL_STRING("casa rossi", norm("  Casa   ROSSI \t"));
    TEST_ASSERT_EQUAL_STRING("kitchen", norm("Kitchen"));
    TEST_ASSERT_EQUAL_STRING("", norm("   "));
    TEST_ASSERT_EQUAL_STRING("", norm(""));
}

/** A name typed with an accent or an emoji must survive byte-for-byte. */
void test_normalise_leaves_non_ascii_alone(void) {
    TEST_ASSERT_EQUAL_STRING("caffè", norm("Caffè"));
}

void test_normalise_never_overruns_and_always_terminates(void) {
    char out[8];
    memset(out, 'x', sizeof(out));
    const size_t n = meshNormaliseName("abcdefghijklmnop", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(7u, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("abcdefg", out);

    // A zero-sized buffer must not be written to at all.
    char guard = 'g';
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)meshNormaliseName("abc", &guard, 0));
    TEST_ASSERT_EQUAL_INT('g', guard);

    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)meshNormaliseName(nullptr, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

// ---------------------------------------------------------------------------
// Id derivation
// ---------------------------------------------------------------------------

/** The whole point: two people typing the same name land in the same mesh. */
void test_the_same_name_typed_differently_is_the_same_mesh(void) {
    const uint16_t id = meshIdFromName("casa rossi");
    TEST_ASSERT_EQUAL_UINT16(id, meshIdFromName("Casa Rossi"));
    TEST_ASSERT_EQUAL_UINT16(id, meshIdFromName("  CASA   rossi  "));
    TEST_ASSERT_EQUAL_UINT16(id, meshIdFromName("casa\trossi"));
}

void test_different_names_are_different_meshes(void) {
    TEST_ASSERT_NOT_EQUAL(meshIdFromName("casa rossi"), meshIdFromName("kitchen"));
    TEST_ASSERT_NOT_EQUAL(meshIdFromName("sonoloco"), meshIdFromName("sonoloco2"));
    // One character apart, because folding both halves of the hash together is
    // what makes this hold; truncating to the low 16 bits would not.
    TEST_ASSERT_NOT_EQUAL(meshIdFromName("casa rossa"), meshIdFromName("casa rossi"));
}

void test_an_empty_name_is_unset(void) {
    TEST_ASSERT_EQUAL_UINT16(MESH_ID_UNSET, meshIdFromName(""));
    TEST_ASSERT_EQUAL_UINT16(MESH_ID_UNSET, meshIdFromName("   \t\n"));
    TEST_ASSERT_EQUAL_UINT16(MESH_ID_UNSET, meshIdFromName(nullptr));
}

/**
 * Unset means unset. A real name that hashed to zero would make its own
 * household's nodes treat each other as unconfigured, which is the one value
 * this function may never return.
 */
void test_a_real_name_is_never_unset(void) {
    char name[16];
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof(name), "mesh-%d", i);
        TEST_ASSERT_NOT_EQUAL(MESH_ID_UNSET, meshIdFromName(name));
    }
}

/**
 * A name past MESH_NAME_MAX hashes as its stored, truncated form.
 *
 * NVS keeps the truncated name; if the hash were taken over the full string,
 * a node re-deriving its id from what it stored would get a different answer
 * after a reboot than it had before one.
 */
void test_a_long_name_hashes_as_what_gets_stored(void) {
    const char *full  = "a-very-long-mesh-name-that-goes-past-the-limit";
    const char *kept  = "a-very-long-mesh-name-that-goes";   // 31 chars
    TEST_ASSERT_EQUAL_UINT(MESH_NAME_MAX, (unsigned)strlen(kept));
    TEST_ASSERT_EQUAL_UINT16(meshIdFromName(kept), meshIdFromName(full));
}

/**
 * Pinned values, not just self-consistency.
 *
 * Everything above would still pass if the hash changed, and a changed hash is
 * a firmware update that silently splits every mesh in the field: the upgraded
 * boards compute one id from the stored name, the ones not yet updated another,
 * and nothing reports an error. If this test fails, the compatibility break is
 * the finding — decide about it deliberately.
 */
void test_known_names_keep_their_ids(void) {
    TEST_ASSERT_EQUAL_UINT16(0xCF09, meshIdFromName("sonoloco"));   // the default
    TEST_ASSERT_EQUAL_UINT16(0x6F59, meshIdFromName("Casa Rossi"));
    TEST_ASSERT_EQUAL_UINT16(0x64C9, meshIdFromName("kitchen"));
}

// ---------------------------------------------------------------------------
// Pairing beacons
// ---------------------------------------------------------------------------

void test_a_beacon_is_the_magic_and_no_payload(void) {
    TEST_ASSERT_TRUE(meshIsBeacon(MESH_BEACON_SEQ, 0));
}

/**
 * One audio packet in every 65536 carries MESH_BEACON_SEQ as its sequence
 * number. At 220 packets/s that is a false beacon every five minutes, in the
 * one code path whose job is to not join the wrong mesh -- so the payload
 * length has to be part of the test.
 */
void test_a_full_packet_is_never_a_beacon(void) {
    TEST_ASSERT_FALSE(meshIsBeacon(MESH_BEACON_SEQ, 200));
    TEST_ASSERT_FALSE(meshIsBeacon(MESH_BEACON_SEQ, 1));
}

/** And a truncated or empty frame is not an invitation either. */
void test_an_empty_packet_alone_is_not_a_beacon(void) {
    TEST_ASSERT_FALSE(meshIsBeacon(0, 0));
    TEST_ASSERT_FALSE(meshIsBeacon(1234, 0));
    TEST_ASSERT_FALSE(meshIsBeacon(MESH_BEACON_SEQ - 1, 0));
}

// ---------------------------------------------------------------------------
// Update requests
// ---------------------------------------------------------------------------

void test_an_update_request_is_the_magic_and_a_target(void) {
    TEST_ASSERT_TRUE(meshIsUpdateRequest(MESH_UPDATE_SEQ, 1));
    TEST_ASSERT_TRUE(meshIsUpdateRequest(MESH_UPDATE_SEQ, MESH_TARGET_MAX));
    TEST_ASSERT_FALSE(meshIsUpdateRequest(MESH_UPDATE_SEQ, 0));
    TEST_ASSERT_FALSE(meshIsUpdateRequest(MESH_UPDATE_SEQ, MESH_TARGET_MAX + 1));
    TEST_ASSERT_FALSE(meshIsUpdateRequest(MESH_BEACON_SEQ, 12));
}

static bool targets(const char *target, const char *room, const char *mac) {
    return meshTargetMatches(target, strlen(target), room, mac);
}

// Made up, and locally administered (0x02 set in the first byte), so it can
// be no real board's: the repo is public.
static const char *MAC = "0A:1B:2C:3D:4E:5F";

/** The build says SonoLoco-C3 and the hostname sonoloco-c3: the same node. */
void test_a_target_is_the_room_name_in_any_case(void) {
    TEST_ASSERT_TRUE(targets("SonoLoco-C3", "SonoLoco-C3", MAC));
    TEST_ASSERT_TRUE(targets("sonoloco-c3", "SonoLoco-C3", MAC));
    TEST_ASSERT_FALSE(targets("SonoLoco-S3", "SonoLoco-C3", MAC));
}

void test_a_target_is_the_mac_in_any_case(void) {
    TEST_ASSERT_TRUE(targets("0A:1B:2C:3D:4E:5F", "SonoLoco-WROVER2", MAC));
    TEST_ASSERT_TRUE(targets("0a:1b:2c:3d:4e:5f", "SonoLoco-WROVER2", MAC));
    TEST_ASSERT_FALSE(targets("0A:1B:2C:3D:4E:60", "SonoLoco-WROVER2", MAC));
}

/**
 * Whole names only. With a prefix match, updating SonoLoco-WROVER would also
 * take SonoLoco-WROVER2 off the mesh, and the other way round with a suffix.
 */
void test_a_target_never_matches_part_of_a_name(void) {
    TEST_ASSERT_FALSE(targets("SonoLoco-WROVER", "SonoLoco-WROVER2", MAC));
    TEST_ASSERT_FALSE(targets("SonoLoco-WROVER2", "SonoLoco-WROVER", MAC));
    TEST_ASSERT_FALSE(targets("0A:1B:2C", "SonoLoco-WROVER2", MAC));
}

/** Nothing on the air may reboot a node without naming it. */
void test_an_empty_or_oversized_target_matches_nothing(void) {
    TEST_ASSERT_FALSE(meshTargetMatches("", 0, "", ""));
    TEST_ASSERT_FALSE(meshTargetMatches("SonoLoco-C3", 0, "SonoLoco-C3", MAC));
    TEST_ASSERT_FALSE(targets("*", "SonoLoco-C3", MAC));
    TEST_ASSERT_FALSE(meshTargetMatches("SonoLoco-C3", 11, nullptr, nullptr));

    char longName[MESH_TARGET_MAX + 2];
    memset(longName, 'a', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    TEST_ASSERT_FALSE(targets(longName, longName, MAC));
}

/** The payload has no terminator; only `len` bytes of it may be read. */
void test_a_target_is_read_to_its_length_only(void) {
    const char wire[] = {'S', 'o', 'n', 'o', 'L', 'o', 'c', 'o', '-', 'C', '3', 'X', 'Y'};
    TEST_ASSERT_TRUE(meshTargetMatches(wire, 11, "SonoLoco-C3", MAC));
    TEST_ASSERT_FALSE(meshTargetMatches(wire, 12, "SonoLoco-C3", MAC));
}

// ---------------------------------------------------------------------------
// Commands over the mesh
// ---------------------------------------------------------------------------

static bool split(const char *line, const char *wantTarget, const char *wantText) {
    const char *t = nullptr, *c = nullptr;
    size_t tl = 0, cl = 0;
    if (!meshSplitCommandLine(line, &t, &tl, &c, &cl)) return false;
    return tl == strlen(wantTarget) && memcmp(t, wantTarget, tl) == 0 &&
           cl == strlen(wantText) && memcmp(c, wantText, cl) == 0;
}

/** What follows `@` on the serial line: a target, a space, the command. */
void test_a_command_line_is_a_target_and_a_command(void) {
    TEST_ASSERT_TRUE(split("SonoLoco-C3 v-20", "SonoLoco-C3", "v-20"));
    TEST_ASSERT_TRUE(split("* N", "*", "N"));
    TEST_ASSERT_TRUE(split("  SonoLoco-C3   v-20  \r", "SonoLoco-C3", "v-20"));
    // An argument keeps its own spaces: `g` takes a mesh name, and one may have them.
    TEST_ASSERT_TRUE(split("SonoLoco-C3 gcasa  rossi", "SonoLoco-C3", "gcasa  rossi"));
}

void test_a_command_line_needs_both_halves(void) {
    const char *t, *c;
    size_t tl, cl;
    TEST_ASSERT_FALSE(meshSplitCommandLine("", &t, &tl, &c, &cl));
    TEST_ASSERT_FALSE(meshSplitCommandLine("SonoLoco-C3", &t, &tl, &c, &cl));
    TEST_ASSERT_FALSE(meshSplitCommandLine("SonoLoco-C3   ", &t, &tl, &c, &cl));
    TEST_ASSERT_FALSE(meshSplitCommandLine(nullptr, &t, &tl, &c, &cl));

    char longTarget[MESH_TARGET_MAX + 8];
    memset(longTarget, 'a', MESH_TARGET_MAX + 1);
    strcpy(longTarget + MESH_TARGET_MAX + 1, " ?");
    TEST_ASSERT_FALSE(meshSplitCommandLine(longTarget, &t, &tl, &c, &cl));

    char longText[MESH_COMMAND_MAX + 8] = "X ";
    memset(longText + 2, 'v', MESH_COMMAND_MAX + 1);
    longText[MESH_COMMAND_MAX + 3] = '\0';
    TEST_ASSERT_FALSE(meshSplitCommandLine(longText, &t, &tl, &c, &cl));
    longText[MESH_COMMAND_MAX + 2] = '\0';   // exactly the limit
    TEST_ASSERT_TRUE(meshSplitCommandLine(longText, &t, &tl, &c, &cl));
}

void test_a_command_survives_the_air(void) {
    uint8_t wire[128];
    const size_t n = meshPackCommand(wire, sizeof(wire), 0xBEEF, "SonoLoco-C3", 11, "v-20", 4);
    TEST_ASSERT_EQUAL(3 + 11 + 4, n);

    MeshCommand cmd;
    TEST_ASSERT_TRUE(meshUnpackCommand(wire, n, &cmd));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, cmd.id);
    TEST_ASSERT_EQUAL(11, cmd.targetLen);
    TEST_ASSERT_EQUAL_MEMORY("SonoLoco-C3", cmd.target, 11);
    TEST_ASSERT_EQUAL(4, cmd.textLen);
    TEST_ASSERT_EQUAL_MEMORY("v-20", cmd.text, 4);
}

/** A request that does not fit is not sent cut short: half a command is another command. */
void test_a_command_that_does_not_fit_is_not_packed(void) {
    uint8_t wire[16];
    TEST_ASSERT_EQUAL(0, meshPackCommand(wire, sizeof(wire), 1, "SonoLoco-C3", 11, "v-20", 4));
    TEST_ASSERT_EQUAL(0, meshPackCommand(wire, sizeof(wire), 1, "", 0, "?", 1));
    TEST_ASSERT_EQUAL(0, meshPackCommand(wire, sizeof(wire), 1, "*", 1, "", 0));
}

void test_a_malformed_command_is_refused(void) {
    MeshCommand cmd;
    const uint8_t noText[] = {1, 0, 2, 'C', '3'};
    TEST_ASSERT_FALSE(meshUnpackCommand(noText, sizeof(noText), &cmd));
    const uint8_t noTarget[] = {1, 0, 0, '?'};
    TEST_ASSERT_FALSE(meshUnpackCommand(noTarget, sizeof(noTarget), &cmd));
    const uint8_t targetPastTheEnd[] = {1, 0, 9, 'C', '3', '?'};
    TEST_ASSERT_FALSE(meshUnpackCommand(targetPastTheEnd, sizeof(targetPastTheEnd), &cmd));
    TEST_ASSERT_FALSE(meshUnpackCommand(noText, 2, &cmd));

    uint8_t tooLong[3 + 1 + MESH_COMMAND_MAX + 1] = {1, 0, 1, '*'};
    memset(tooLong + 4, 'v', sizeof(tooLong) - 4);
    TEST_ASSERT_FALSE(meshUnpackCommand(tooLong, sizeof(tooLong), &cmd));
    TEST_ASSERT_TRUE(meshUnpackCommand(tooLong, sizeof(tooLong) - 1, &cmd));
}

void test_a_reply_survives_the_air(void) {
    uint8_t wire[64];
    const char text[] = "[OUT] trim=-20dB\n";
    const size_t n = meshPackReply(wire, sizeof(wire), 0x1234, 2 | MESH_REPLY_CONT, "SonoLoco-C3",
                                   text, strlen(text));
    TEST_ASSERT_EQUAL(MESH_REPLY_HEADER(11) + strlen(text), n);

    MeshReply r;
    TEST_ASSERT_TRUE(meshUnpackReply(wire, n, &r));
    TEST_ASSERT_EQUAL_HEX16(0x1234, r.id);
    TEST_ASSERT_EQUAL_HEX8(2 | MESH_REPLY_CONT, r.part);
    TEST_ASSERT_EQUAL(11, r.nameLen);
    TEST_ASSERT_EQUAL_MEMORY("SonoLoco-C3", r.name, 11);
    TEST_ASSERT_EQUAL(strlen(text), r.textLen);
    TEST_ASSERT_EQUAL_MEMORY(text, r.text, r.textLen);
}

/** A command that printed nothing still answers, so the asker knows it ran. */
void test_an_empty_reply_is_still_a_reply(void) {
    uint8_t wire[32];
    const size_t n = meshPackReply(wire, sizeof(wire), 7, 0, "S3", "", 0);
    MeshReply r;
    TEST_ASSERT_TRUE(meshUnpackReply(wire, n, &r));
    TEST_ASSERT_EQUAL(0, r.textLen);

    const uint8_t noName[] = {7, 0, 0, 0, 'x'};
    TEST_ASSERT_FALSE(meshUnpackReply(noName, sizeof(noName), &r));
    const uint8_t namePastTheEnd[] = {7, 0, 0, 5, 'S', '3'};
    TEST_ASSERT_FALSE(meshUnpackReply(namePastTheEnd, sizeof(namePastTheEnd), &r));
}

/** Parts end at a line's end where they can, so a reply reads as whole lines. */
void test_a_reply_is_cut_at_line_ends(void) {
    const char *text = "first line\nsecond line\nthird";
    TEST_ASSERT_EQUAL(strlen(text), meshReplyChunk(text, strlen(text), 100));
    TEST_ASSERT_EQUAL(23, meshReplyChunk(text, strlen(text), 25));   // after "second line\n"
    TEST_ASSERT_EQUAL(11, meshReplyChunk(text, strlen(text), 22));   // "second line\n" ends at 23
    TEST_ASSERT_EQUAL(11, meshReplyChunk(text, strlen(text), 11));   // the newline is the last byte
}

/** Only a line longer than a whole part is cut in the middle. */
void test_a_line_longer_than_a_part_is_cut_where_it_must_be(void) {
    const char *text = "abcdefghijklmnop\nq";
    TEST_ASSERT_EQUAL(8, meshReplyChunk(text, strlen(text), 8));
}

/** An answer sent again is the same part of the same node's answer, and printed once. */
void test_an_answer_sent_again_is_recognised(void) {
    const uint32_t k = meshReplyKey("SonoLoco-C3", 11, 0);
    TEST_ASSERT_EQUAL_HEX32(k, meshReplyKey("sonoloco-c3", 11, 0));
    TEST_ASSERT_NOT_EQUAL(k, meshReplyKey("SonoLoco-C3", 11, 1));
    TEST_ASSERT_NOT_EQUAL(k, meshReplyKey("SonoLoco-C3", 11, MESH_REPLY_CONT));
    TEST_ASSERT_NOT_EQUAL(k, meshReplyKey("SonoLoco-S3", 11, 0));
}

/** The password goes nowhere near the air, and no node relays for another. */
void test_some_commands_never_leave_the_node(void) {
    TEST_ASSERT_FALSE(meshCommandAllowedRemote('W'));
    TEST_ASSERT_FALSE(meshCommandAllowedRemote('@'));
    TEST_ASSERT_FALSE(meshCommandAllowedRemote('\0'));
    TEST_ASSERT_TRUE(meshCommandAllowedRemote('v'));
    TEST_ASSERT_TRUE(meshCommandAllowedRemote('U'));
}

/** Sent to `*`, anything that reboots a node or takes it off the mesh would take them all. */
void test_nothing_sent_to_all_can_take_the_house_off_the_air(void) {
    for (const char *c = "UbncgpowW@"; *c; c++) TEST_ASSERT_FALSE(meshCommandAllowedForAll(*c));
    for (const char *c = "?NrvmlM"; *c; c++) TEST_ASSERT_TRUE(meshCommandAllowedForAll(*c));
}

void test_a_command_is_for_its_target_or_for_all(void) {
    TEST_ASSERT_TRUE(meshCommandAddressed("*", 1, 'N', "SonoLoco-C3", MAC));
    TEST_ASSERT_FALSE(meshCommandAddressed("*", 1, 'U', "SonoLoco-C3", MAC));
    TEST_ASSERT_TRUE(meshCommandAddressed("sonoloco-c3", 11, 'U', "SonoLoco-C3", MAC));
    TEST_ASSERT_TRUE(meshCommandAddressed(MAC, strlen(MAC), 'v', "SonoLoco-C3", MAC));
    TEST_ASSERT_FALSE(meshCommandAddressed("SonoLoco-S3", 11, 'N', "SonoLoco-C3", MAC));
    TEST_ASSERT_FALSE(meshCommandAddressed("SonoLoco-C3", 11, 'W', "SonoLoco-C3", MAC));
    // `*` is a target for commands only: an update request naming it matches nothing.
    TEST_ASSERT_FALSE(targets("*", "SonoLoco-C3", MAC));
}

// ---------------------------------------------------------------------------

int runAllTests(void) {
    UNITY_BEGIN();

    RUN_TEST(test_normalise_trims_lowercases_and_collapses);
    RUN_TEST(test_normalise_leaves_non_ascii_alone);
    RUN_TEST(test_normalise_never_overruns_and_always_terminates);

    RUN_TEST(test_the_same_name_typed_differently_is_the_same_mesh);
    RUN_TEST(test_different_names_are_different_meshes);
    RUN_TEST(test_an_empty_name_is_unset);
    RUN_TEST(test_a_real_name_is_never_unset);
    RUN_TEST(test_a_long_name_hashes_as_what_gets_stored);
    RUN_TEST(test_known_names_keep_their_ids);

    RUN_TEST(test_a_beacon_is_the_magic_and_no_payload);
    RUN_TEST(test_a_full_packet_is_never_a_beacon);
    RUN_TEST(test_an_empty_packet_alone_is_not_a_beacon);

    RUN_TEST(test_an_update_request_is_the_magic_and_a_target);
    RUN_TEST(test_a_target_is_the_room_name_in_any_case);
    RUN_TEST(test_a_target_is_the_mac_in_any_case);
    RUN_TEST(test_a_target_never_matches_part_of_a_name);
    RUN_TEST(test_an_empty_or_oversized_target_matches_nothing);
    RUN_TEST(test_a_target_is_read_to_its_length_only);

    RUN_TEST(test_a_command_line_is_a_target_and_a_command);
    RUN_TEST(test_a_command_line_needs_both_halves);
    RUN_TEST(test_a_command_survives_the_air);
    RUN_TEST(test_a_command_that_does_not_fit_is_not_packed);
    RUN_TEST(test_a_malformed_command_is_refused);
    RUN_TEST(test_a_reply_survives_the_air);
    RUN_TEST(test_an_empty_reply_is_still_a_reply);
    RUN_TEST(test_a_reply_is_cut_at_line_ends);
    RUN_TEST(test_a_line_longer_than_a_part_is_cut_where_it_must_be);
    RUN_TEST(test_an_answer_sent_again_is_recognised);
    RUN_TEST(test_some_commands_never_leave_the_node);
    RUN_TEST(test_nothing_sent_to_all_can_take_the_house_off_the_air);
    RUN_TEST(test_a_command_is_for_its_target_or_for_all);

    return UNITY_END();
}

#ifdef ARDUINO

// Same tests on the board, as with test_jitter and test_drift: there is no host
// compiler on this machine yet, and `char` is signed on Xtensa and RISC-V alike
// here but the hash reads bytes unsigned on purpose — worth asserting on the
// target rather than assuming.
#include <Arduino.h>

void setup() {
    delay(2000);   // let the host's serial monitor attach before output starts
    runAllTests();
}

void loop() {}

#else

int main(int, char **) {
    return runAllTests();
}

#endif
