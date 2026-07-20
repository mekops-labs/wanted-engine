/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <wanted-autoconf.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * kconfig — the build-configuration round trip.
 *
 * Guards the mechanism rather than any one symbol: that the Kconfig tree is
 * read, that a header is generated from .config, and that it reaches a
 * translation unit through the include path. A build whose configuration step
 * silently did nothing fails here rather than much later, as a feature that
 * quietly compiled out.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(kconfig);

TEST_SETUP(kconfig) {}
TEST_TEAR_DOWN(kconfig) {}

TEST(kconfig, GeneratedHeaderIsReachable) {
#ifndef CONFIG_WANTED_KCONFIG_SELFTEST
    TEST_FAIL_MESSAGE("wanted-autoconf.h reached this TU but the self-test "
                      "symbol is absent: .config and the generated header "
                      "disagree, or the symbol was dropped from Kconfig");
#endif
}

TEST_GROUP_RUNNER(kconfig) { RUN_TEST_CASE(kconfig, GeneratedHeaderIsReachable); }
