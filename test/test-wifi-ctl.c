/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <wifi-ctl.h>

/* The grammar of `connect` on /dev/wifi/ctl: a bare or quoted SSID, then an
 * optional passphrase. */

TEST_GROUP(wifi_ctl);

TEST_SETUP(wifi_ctl) {}
TEST_TEAR_DOWN(wifi_ctl) {}

static int split(char *args, char **ssid, char **pass) {
    return WifiCtlSplitConnect(args, ssid, pass);
}

TEST(wifi_ctl, ABareSsidAndAPassphrase) {
    char buf[] = " home secret123";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("home", ssid);
    TEST_ASSERT_EQUAL_STRING("secret123", pass);
}

TEST(wifi_ctl, ABareSsidAloneIsAnOpenNetwork) {
    char buf[] = " open";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("open", ssid);
    TEST_ASSERT_EQUAL_STRING("", pass);
}

TEST(wifi_ctl, ThePassphraseKeepsItsSpaces) {
    char buf[] = " home two words";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("home", ssid);
    TEST_ASSERT_EQUAL_STRING("two words", pass);
}

TEST(wifi_ctl, NothingAfterConnectIsRefused) {
    char a[] = "";
    char b[] = "   ";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(-EINVAL, split(a, &ssid, &pass));
    TEST_ASSERT_EQUAL_INT(-EINVAL, split(b, &ssid, &pass));
}

TEST(wifi_ctl, AQuotedSsidMayHoldSpaces) {
    char buf[] = " \"my net\" secret123";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("my net", ssid);
    TEST_ASSERT_EQUAL_STRING("secret123", pass);
}

TEST(wifi_ctl, AQuotedSsidAloneIsAnOpenNetwork) {
    char buf[] = " \"my net\"";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("my net", ssid);
    TEST_ASSERT_EQUAL_STRING("", pass);
}

TEST(wifi_ctl, ThePassphraseAfterAQuotedSsidKeepsItsSpaces) {
    char buf[] = " \"my net\" two words";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("two words", pass);
}

TEST(wifi_ctl, AQuoteAndABackslashAreEscaped) {
    char buf[] = " \"a\\\"b\\\\c\" pw";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("a\"b\\c", ssid);
    TEST_ASSERT_EQUAL_STRING("pw", pass);
}

TEST(wifi_ctl, AQuotedSsidMayStartWithAQuote) {
    char buf[] = " \"\\\"x\"";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("\"x", ssid);
}

TEST(wifi_ctl, AnUnterminatedQuoteIsRefused) {
    char buf[] = " \"my net";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(-EINVAL, split(buf, &ssid, &pass));
}

TEST(wifi_ctl, ATrailingBackslashIsRefused) {
    char buf[] = " \"my net\\";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(-EINVAL, split(buf, &ssid, &pass));
}

TEST(wifi_ctl, AnUnknownEscapeIsRefused) {
    char buf[] = " \"a\\nb\"";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(-EINVAL, split(buf, &ssid, &pass));
}

TEST(wifi_ctl, AnEmptyQuotedSsidIsRefused) {
    char buf[] = " \"\" pw";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(-EINVAL, split(buf, &ssid, &pass));
}

TEST(wifi_ctl, TextRightAfterTheClosingQuoteIsRefused) {
    char buf[] = " \"my net\"x";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(-EINVAL, split(buf, &ssid, &pass));
}

TEST(wifi_ctl, AQuoteInsideABareSsidIsPlainText) {
    char buf[] = " a\"b pw";
    char *ssid, *pass;
    TEST_ASSERT_EQUAL_INT(0, split(buf, &ssid, &pass));
    TEST_ASSERT_EQUAL_STRING("a\"b", ssid);
    TEST_ASSERT_EQUAL_STRING("pw", pass);
}

TEST_GROUP_RUNNER(wifi_ctl) {
    RUN_TEST_CASE(wifi_ctl, ABareSsidAndAPassphrase);
    RUN_TEST_CASE(wifi_ctl, ABareSsidAloneIsAnOpenNetwork);
    RUN_TEST_CASE(wifi_ctl, ThePassphraseKeepsItsSpaces);
    RUN_TEST_CASE(wifi_ctl, NothingAfterConnectIsRefused);
    RUN_TEST_CASE(wifi_ctl, AQuotedSsidMayHoldSpaces);
    RUN_TEST_CASE(wifi_ctl, AQuotedSsidAloneIsAnOpenNetwork);
    RUN_TEST_CASE(wifi_ctl, ThePassphraseAfterAQuotedSsidKeepsItsSpaces);
    RUN_TEST_CASE(wifi_ctl, AQuoteAndABackslashAreEscaped);
    RUN_TEST_CASE(wifi_ctl, AQuotedSsidMayStartWithAQuote);
    RUN_TEST_CASE(wifi_ctl, AnUnterminatedQuoteIsRefused);
    RUN_TEST_CASE(wifi_ctl, ATrailingBackslashIsRefused);
    RUN_TEST_CASE(wifi_ctl, AnUnknownEscapeIsRefused);
    RUN_TEST_CASE(wifi_ctl, AnEmptyQuotedSsidIsRefused);
    RUN_TEST_CASE(wifi_ctl, TextRightAfterTheClosingQuoteIsRefused);
    RUN_TEST_CASE(wifi_ctl, AQuoteInsideABareSsidIsPlainText);
}
