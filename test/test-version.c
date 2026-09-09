/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <wanted-version.h>

/***************************************/
TEST_GROUP(version_order);
/***************************************/

TEST_SETUP(version_order) {}

TEST_TEAR_DOWN(version_order) {}

TEST(version_order, HigherReleaseIsNewer) {
    TEST_ASSERT_TRUE(VersionNewer("0.8.0", "0.7.9"));
    TEST_ASSERT_FALSE(VersionNewer("0.7.9", "0.8.0"));
}

TEST(version_order, EachPartIsComparedInTurn) {
    TEST_ASSERT_TRUE(VersionNewer("1.0.0", "0.99.99"));
    TEST_ASSERT_TRUE(VersionNewer("0.7.10", "0.7.9"));
    TEST_ASSERT_FALSE(VersionNewer("0.7.9", "0.7.10"));
}

TEST(version_order, TheSameVersionIsNotNewer) {
    TEST_ASSERT_FALSE(VersionNewer("0.7.0", "0.7.0"));
}

TEST(version_order, ALeadingVIsCosmetic) {
    TEST_ASSERT_FALSE(VersionNewer("v0.7.0", "0.7.0"));
    TEST_ASSERT_TRUE(VersionNewer("v0.8.0", "0.7.0"));
}

/* registry-version.sh writes `<tag>-<commit>` for a build past that tag, so
 * the suffix means newer here — the opposite of a semver pre-release. */
TEST(version_order, ABuildPastTheTagIsNewerThanTheTag) {
    TEST_ASSERT_TRUE(VersionNewer("0.7.0-abc123", "0.7.0"));
    TEST_ASSERT_FALSE(VersionNewer("0.7.0", "0.7.0-abc123"));
}

/* Two commits past one tag carry no ordering, so neither displaces the other
 * and a caller keeps whatever it already runs. */
TEST(version_order, TwoBuildsPastOneTagAreUnordered) {
    TEST_ASSERT_FALSE(VersionNewer("0.7.0-abc123", "0.7.0-def456"));
    TEST_ASSERT_FALSE(VersionNewer("0.7.0-def456", "0.7.0-abc123"));
}

TEST(version_order, AReleaseStillOutranksACommitPastAnOlderTag) {
    TEST_ASSERT_TRUE(VersionNewer("0.8.0", "0.7.0-abc123"));
    TEST_ASSERT_FALSE(VersionNewer("0.7.0-abc123", "0.8.0"));
}

TEST(version_order, AMissingPartReadsAsZero) {
    TEST_ASSERT_TRUE(VersionNewer("0.8", "0.7.9"));
    TEST_ASSERT_FALSE(VersionNewer("0.7", "0.7.1"));
}

/* An unreadable version never displaces one that parses, whichever side it is
 * on: an empty or malformed string must not silently win a comparison. */
TEST(version_order, AnUnreadableVersionNeverDisplacesAReadableOne) {
    TEST_ASSERT_FALSE(VersionNewer("", "0.7.0"));
    TEST_ASSERT_FALSE(VersionNewer(NULL, "0.7.0"));
    TEST_ASSERT_FALSE(VersionNewer("dev", "0.7.0"));
    TEST_ASSERT_TRUE(VersionNewer("0.7.0", ""));
    TEST_ASSERT_TRUE(VersionNewer("0.7.0", NULL));
}

TEST(version_order, TwoUnreadableVersionsAreUnordered) {
    TEST_ASSERT_FALSE(VersionNewer("dev", "dev"));
    TEST_ASSERT_FALSE(VersionNewer(NULL, NULL));
}

TEST_GROUP_RUNNER(version_order) {
    RUN_TEST_CASE(version_order, HigherReleaseIsNewer);
    RUN_TEST_CASE(version_order, EachPartIsComparedInTurn);
    RUN_TEST_CASE(version_order, TheSameVersionIsNotNewer);
    RUN_TEST_CASE(version_order, ALeadingVIsCosmetic);
    RUN_TEST_CASE(version_order, ABuildPastTheTagIsNewerThanTheTag);
    RUN_TEST_CASE(version_order, TwoBuildsPastOneTagAreUnordered);
    RUN_TEST_CASE(version_order, AReleaseStillOutranksACommitPastAnOlderTag);
    RUN_TEST_CASE(version_order, AMissingPartReadsAsZero);
    RUN_TEST_CASE(version_order, AnUnreadableVersionNeverDisplacesAReadableOne);
    RUN_TEST_CASE(version_order, TwoUnreadableVersionsAreUnordered);
}
