/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <dummy-fs.h>
#include <platform.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-lora.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-vfs-api.h>

/* /dev/lora: the grant, the owner claim, the ctl grammar, the frame records,
 * the bands and the duty-cycle account, against a fake backing that records
 * every call it gets. */

#define GRANT_RX "devices=main"
#define GRANT_TX "devices=main,tx"
#define EU_SET "set freq=869525000 sf=11 bw=250 cr=5 sync=0x2B preamble=16"
#define MS 1000000ULL

typedef struct fake_t {
    char log[512];
    int configureRc;
    int transmitRc;
    bool busy;
    lora_params_t last;
    uint8_t sent[LORA_PAYLOAD_MAX];
    size_t sentLen;
} fake_t;

static fake_t fake;

static void note(const char *what) {
    strncat(fake.log, what, sizeof(fake.log) - strlen(fake.log) - 1);
    strncat(fake.log, " ", sizeof(fake.log) - strlen(fake.log) - 1);
}

static int fConfigure(void *ctx, const lora_params_t *p) {
    (void)ctx;
    note("configure");
    if (fake.configureRc == 0)
        fake.last = *p;
    return fake.configureRc;
}
static int fStart(void *ctx) {
    (void)ctx;
    note("start");
    return 0;
}
static int fStop(void *ctx) {
    (void)ctx;
    note("stop");
    return 0;
}
static int fSleep(void *ctx) {
    (void)ctx;
    note("sleep");
    return 0;
}
static int fTransmit(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    note("transmit");
    memcpy(fake.sent, buf, len);
    fake.sentLen = len;
    return fake.transmitRc;
}
static int fCad(void *ctx, bool *active) {
    (void)ctx;
    note("cad");
    *active = fake.busy;
    return 0;
}

static const lora_ops_t fullOps = {fConfigure, fStart,    fStop,
                                   fSleep,     fTransmit, fCad};
static const lora_ops_t rxOnlyOps = {fConfigure, fStart, fStop,
                                     fSleep,     NULL,   NULL};

/* EU 868 g3: 869.4 to 869.65 MHz, 27 dBm, 10 %; and g1 at 1 % and 14 dBm. */
static const lora_band_t euBands[] = {
    {869400000, 869650000, 27, 100},
    {868000000, 868600000, 14, 10},
};

typedef struct peer_t {
    vfs_ctx_t vfs;
    vfs_driver_t *drv;
} peer_t;

static peer_t owner;
static peer_t other;
static lora_device_t *dev;

static void registerMain(const lora_ops_t *ops, bool bands) {
    lora_device_desc_t d = {
        "main", "fake", ops, NULL, bands ? euBands : NULL, bands ? 2 : 0};
    TEST_ASSERT_EQUAL_INT(0, LoraDeviceRegister(&d, &dev));
}

static bool attach(peer_t *p, const char *options) {
    p->vfs = VfsInit();
    p->drv = VfsLoraInit(NULL, options);
    if (p->drv == NULL)
        return false;
    DevFs_Register(p->vfs, "lora", p->drv);
    return true;
}

static void detach(peer_t *p) {
    VfsDestroy(&p->vfs);
    p->drv = NULL;
}

static int openOn(peer_t *p, const char *node, int flags) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/lora/main/%s", node);
    return VfsOpen(p->vfs, path, flags);
}

static int writeFd(peer_t *p, int fd, const char *s) {
    return VfsWrite(p->vfs, fd, s, strlen(s));
}

/* The status node of `p` into `buf`. */
static void status(peer_t *p, char *buf, size_t len) {
    int fd = openOn(p, "status", VFS_O_RDONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    int n = VfsRead(p->vfs, fd, buf, len - 1);
    VfsClose(p->vfs, fd);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, n);
    buf[n] = '\0';
}

static void assertStatus(peer_t *p, const char *line) {
    char buf[512];
    status(p, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, line), buf);
}

static void push(const char *payload, bool crc) {
    lora_rx_frame_t f = {(const uint8_t *)payload, strlen(payload), -1075, 65,
                         crc};
    LoraDevicePush(dev, &f);
}

static void reset(void) {
    memset(&fake, 0, sizeof(fake));
    DummyClockReset();
}

/***************************************/
TEST_GROUP(lora_grant);

TEST_SETUP(lora_grant) {
    reset();
    registerMain(&fullOps, true);
}

TEST_TEAR_DOWN(lora_grant) {
    detach(&other);
    detach(&owner);
    LoraDevicesReset();
}

TEST(lora_grant, GrantExposesTheTree) {
    TEST_ASSERT_TRUE(attach(&owner, GRANT_RX));
    const char *nodes[] = {"rx", "tx", "ctl", "status"};
    int fd = openOn(&owner, "status", VFS_O_RDONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    VfsClose(owner.vfs, fd);
    for (size_t i = 0; i < 3; i++) {
        fd = openOn(&owner, nodes[i], VFS_O_RDONLY);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, fd, nodes[i]);
        VfsClose(owner.vfs, fd);
    }
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(owner.vfs, "/dev/lora/aux/rx", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT, openOn(&owner, "power", VFS_O_RDONLY));
}

TEST(lora_grant, UnknownDeviceOrClauseFailsTheLaunch) {
    const char *bad[] = {"",
                         "devices=",
                         "devices=aux",
                         "devices=main,main",
                         "devices=main,tx,tx",
                         "devices=main,rx",
                         "radios=main"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(attach(&owner, bad[i]), bad[i]);
        detach(&owner);
    }
}

TEST(lora_grant, TxNeedsTheTxClause) {
    TEST_ASSERT_TRUE(attach(&owner, GRANT_RX));
    TEST_ASSERT_EQUAL_INT(-EACCES, openOn(&owner, "tx", VFS_O_WRONLY));
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_TX));
    int fd = openOn(&owner, "tx", VFS_O_WRONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
}

TEST(lora_grant, RegistrationChecksTheDescription) {
    lora_device_desc_t d = {"main", "fake", &fullOps, NULL, NULL, 0};
    TEST_ASSERT_EQUAL_INT(-EEXIST, LoraDeviceRegister(&d, NULL));
    d.name = "bad name";
    TEST_ASSERT_EQUAL_INT(-EINVAL, LoraDeviceRegister(&d, NULL));
    lora_band_t inverted = {869650000, 869400000, 14, 10};
    lora_device_desc_t b = {"aux", "fake", &fullOps, NULL, &inverted, 1};
    TEST_ASSERT_EQUAL_INT(-EINVAL, LoraDeviceRegister(&b, NULL));
}

TEST_GROUP_RUNNER(lora_grant) {
    RUN_TEST_CASE(lora_grant, GrantExposesTheTree);
    RUN_TEST_CASE(lora_grant, UnknownDeviceOrClauseFailsTheLaunch);
    RUN_TEST_CASE(lora_grant, TxNeedsTheTxClause);
    RUN_TEST_CASE(lora_grant, RegistrationChecksTheDescription);
}

/***************************************/
TEST_GROUP(lora_owner);

TEST_SETUP(lora_owner) {
    reset();
    registerMain(&fullOps, true);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_TX));
    TEST_ASSERT_TRUE(attach(&other, GRANT_TX));
}

TEST_TEAR_DOWN(lora_owner) {
    detach(&other);
    detach(&owner);
    LoraDevicesReset();
}

TEST(lora_owner, FirstOpenTakesTheRadioAndWakesIt) {
    assertStatus(&other, "owner=none");
    assertStatus(&other, "state=sleep");
    int fd = openOn(&owner, "ctl", VFS_O_WRONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    TEST_ASSERT_EQUAL_STRING("stop ", fake.log);
    assertStatus(&other, "owner=held");
    assertStatus(&other, "state=standby");
    TEST_ASSERT_EQUAL_INT(-EBUSY, openOn(&other, "rx", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-EBUSY, openOn(&other, "tx", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EBUSY, openOn(&other, "ctl", VFS_O_WRONLY));
    /* The owner opens more nodes freely. */
    int rx = openOn(&owner, "rx", VFS_O_RDONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, rx);
    TEST_ASSERT_EQUAL_STRING("stop ", fake.log);
}

TEST(lora_owner, LastCloseSleepsTheChipAndForgetsTheSettings) {
    int ctl = openOn(&owner, "ctl", VFS_O_WRONLY);
    int rx = openOn(&owner, "rx", VFS_O_RDONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, writeFd(&owner, ctl, EU_SET));
    VfsClose(owner.vfs, ctl);
    assertStatus(&other, "owner=held");
    VfsClose(owner.vfs, rx);
    assertStatus(&other, "owner=none");
    assertStatus(&other, "state=sleep");
    TEST_ASSERT_NOT_NULL(strstr(fake.log, "sleep"));

    /* A new owner needs a set before start. */
    ctl = openOn(&other, "ctl", VFS_O_WRONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ctl);
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeFd(&other, ctl, "start"));
    char buf[512];
    status(&other, buf, sizeof(buf));
    TEST_ASSERT_NULL(strstr(buf, "freq="));
}

TEST(lora_owner, DestroyReleasesTheRadio) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, openOn(&owner, "rx", VFS_O_RDONLY));
    detach(&owner);
    assertStatus(&other, "owner=none");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, openOn(&other, "rx", VFS_O_RDONLY));
}

TEST_GROUP_RUNNER(lora_owner) {
    RUN_TEST_CASE(lora_owner, FirstOpenTakesTheRadioAndWakesIt);
    RUN_TEST_CASE(lora_owner, LastCloseSleepsTheChipAndForgetsTheSettings);
    RUN_TEST_CASE(lora_owner, DestroyReleasesTheRadio);
}

/***************************************/
TEST_GROUP(lora_ctl);

static int ctl;

TEST_SETUP(lora_ctl) {
    reset();
    registerMain(&fullOps, true);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_TX));
    ctl = openOn(&owner, "ctl", VFS_O_WRONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ctl);
    fake.log[0] = '\0';
}

TEST_TEAR_DOWN(lora_ctl) {
    detach(&owner);
    LoraDevicesReset();
}

TEST(lora_ctl, SetAppliesEverySetting) {
    TEST_ASSERT_EQUAL_INT((int)strlen(EU_SET " power=20 cad=1\n"),
                          writeFd(&owner, ctl, EU_SET " power=20 cad=1\n"));
    TEST_ASSERT_EQUAL_UINT32(869525000, fake.last.freq_hz);
    TEST_ASSERT_EQUAL_UINT32(250000, fake.last.bw_hz);
    TEST_ASSERT_EQUAL_UINT8(11, fake.last.sf);
    TEST_ASSERT_EQUAL_UINT8(5, fake.last.cr);
    TEST_ASSERT_EQUAL_UINT8(0x2B, fake.last.sync);
    TEST_ASSERT_EQUAL_INT8(20, fake.last.power_dbm);
    TEST_ASSERT_EQUAL_UINT16(16, fake.last.preamble);
    const char *lines[] = {"freq=869525000\n", "sf=11\n",     "bw=250.000\n",
                           "cr=5\n",           "sync=0x2B\n", "power=20\n",
                           "preamble=16\n",    "cad=1\n",     "chip=fake\n"};
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        assertStatus(&owner, lines[i]);
}

TEST(lora_ctl, DefaultsAndTheBandLimitOnPower) {
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=869525000 sf=7 bw=125 power=30"));
    TEST_ASSERT_EQUAL_INT8(27, fake.last.power_dbm);
    TEST_ASSERT_EQUAL_UINT8(0x12, fake.last.sync);
    TEST_ASSERT_EQUAL_UINT8(5, fake.last.cr);
    TEST_ASSERT_EQUAL_UINT16(8, fake.last.preamble);
    /* Without power, the band's limit applies. */
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=868100000 sf=7 bw=125"));
    TEST_ASSERT_EQUAL_INT8(14, fake.last.power_dbm);
    assertStatus(&owner, "power=14\n");
}

TEST(lora_ctl, BandwidthTakesDecimals) {
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=869525000 sf=12 bw=62.5"));
    TEST_ASSERT_EQUAL_UINT32(62500, fake.last.bw_hz);
    assertStatus(&owner, "bw=62.500\n");
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=869525000 sf=12 bw=31.25"));
    TEST_ASSERT_EQUAL_UINT32(31250, fake.last.bw_hz);
}

TEST(lora_ctl, MalformedCommandsChangeNothing) {
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, EU_SET));
    fake.log[0] = '\0';
    const char *bad[] = {
        "set sf=11 bw=250",
        "set freq=869525000 bw=250",
        "set freq=869525000 sf=11",
        "set freq=869525000 sf=4 bw=250",
        "set freq=869525000 sf=13 bw=250",
        "set freq=869525000 sf=11 bw=250 cr=9",
        "set freq=869525000 sf=11 bw=250 sync=2B",
        "set freq=869525000 sf=11 bw=250 sync=0x123",
        "set freq=869525000 sf=11 bw=250 preamble=0",
        "set freq=869525000 sf=11 bw=250 cad=2",
        "set freq=869525000 sf=11 bw=250 sf=11",
        "set freq=869525000 sf=11 bw=250 band=1",
        "set freq=869525000 sf=11 bw=0",
        "set freq=869525000 sf=11 bw=62.5555",
        "set freq=869525000\tsf=11 bw=250",
        "start now",
        "listen",
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(-EINVAL, writeFd(&owner, ctl, bad[i]),
                                      bad[i]);
    TEST_ASSERT_EQUAL_STRING("", fake.log);
    assertStatus(&owner, "sf=11\n");
}

TEST(lora_ctl, ChannelOutsideEveryBandReceivesButCannotSend) {
    int tx = openOn(&owner, "tx", VFS_O_WRONLY);
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=915000000 sf=7 bw=125"));
    TEST_ASSERT_EQUAL_INT8(0, fake.last.power_dbm);
    TEST_ASSERT_EQUAL_INT(5, writeFd(&owner, ctl, "start"));
    assertStatus(&owner, "state=rx\n");
    fake.log[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-EPERM, writeFd(&owner, tx, "hi"));
    TEST_ASSERT_EQUAL_STRING("", fake.log);
    /* Both edges must fit: 869.6 MHz at 125 kHz ends at 869.6625 MHz. */
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=869600000 sf=7 bw=125"));
    fake.log[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-EPERM, writeFd(&owner, tx, "hi"));
    TEST_ASSERT_EQUAL_STRING("", fake.log);
}

TEST(lora_ctl, ChipRefusalLeavesTheSettings) {
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, EU_SET));
    fake.configureRc = -EINVAL;
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, writeFd(&owner, ctl, "set freq=869525000 sf=11 bw=7.8"));
    assertStatus(&owner, "bw=250.000\n");
    fake.configureRc = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, writeFd(&owner, ctl, EU_SET));
}

TEST(lora_ctl, StartAndStopMoveTheState) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeFd(&owner, ctl, "start"));
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, EU_SET));
    TEST_ASSERT_EQUAL_INT(5, writeFd(&owner, ctl, "start"));
    assertStatus(&owner, "state=rx\n");
    /* A set while receiving stops, applies and resumes. */
    fake.log[0] = '\0';
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, EU_SET));
    TEST_ASSERT_EQUAL_STRING("stop configure start ", fake.log);
    assertStatus(&owner, "state=rx\n");
    TEST_ASSERT_EQUAL_INT(5, writeFd(&owner, ctl, "stop\n"));
    assertStatus(&owner, "state=standby\n");
}

TEST(lora_ctl, CadNeedsDetection) {
    detach(&owner);
    LoraDevicesReset();
    registerMain(&rxOnlyOps, true);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_TX));
    ctl = openOn(&owner, "ctl", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-ENOTSUP, writeFd(&owner, ctl, EU_SET " cad=1"));
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, EU_SET " cad=0"));
}

TEST_GROUP_RUNNER(lora_ctl) {
    RUN_TEST_CASE(lora_ctl, SetAppliesEverySetting);
    RUN_TEST_CASE(lora_ctl, DefaultsAndTheBandLimitOnPower);
    RUN_TEST_CASE(lora_ctl, BandwidthTakesDecimals);
    RUN_TEST_CASE(lora_ctl, MalformedCommandsChangeNothing);
    RUN_TEST_CASE(lora_ctl, ChannelOutsideEveryBandReceivesButCannotSend);
    RUN_TEST_CASE(lora_ctl, ChipRefusalLeavesTheSettings);
    RUN_TEST_CASE(lora_ctl, StartAndStopMoveTheState);
    RUN_TEST_CASE(lora_ctl, CadNeedsDetection);
}

/***************************************/
TEST_GROUP(lora_rx);

static int rxFd;

TEST_SETUP(lora_rx) {
    reset();
    registerMain(&fullOps, true);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_RX));
    ctl = openOn(&owner, "ctl", VFS_O_WRONLY);
    rxFd = openOn(&owner, "rx", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, EU_SET));
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, "start"));
}

TEST_TEAR_DOWN(lora_rx) {
    detach(&owner);
    LoraDevicesReset();
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

TEST(lora_rx, RecordCarriesTheHeaderAndPayload) {
    DummyClockAdvance(1234 * MS);
    push("hello", true);
    uint8_t buf[300];
    int n = VfsRead(owner.vfs, rxFd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(LORA_RECORD_HEADER + 5, n);
    TEST_ASSERT_EQUAL_UINT8(1, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[1]);
    TEST_ASSERT_EQUAL_UINT16(5, buf[2] | buf[3] << 8);
    TEST_ASSERT_EQUAL_INT16(-1075, (int16_t)(buf[4] | buf[5] << 8));
    TEST_ASSERT_EQUAL_INT16(65, (int16_t)(buf[6] | buf[7] << 8));
    TEST_ASSERT_EQUAL_UINT32(0, le32(buf + 8));
    TEST_ASSERT_EQUAL_MEMORY("hello", buf + LORA_RECORD_HEADER, 5);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(owner.vfs, rxFd, buf, sizeof(buf)));
}

TEST(lora_rx, ReadReturnsWholeRecordsOnly) {
    push("abc", true);
    push("defg", true);
    uint8_t buf[300];
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsRead(owner.vfs, rxFd, buf, 22));
    /* One record fits in 30 bytes, not two. */
    TEST_ASSERT_EQUAL_INT(23, VfsRead(owner.vfs, rxFd, buf, 30));
    TEST_ASSERT_EQUAL_INT(24, VfsRead(owner.vfs, rxFd, buf, sizeof(buf)));
}

TEST(lora_rx, BadCrcAndIdleRadioQueueNothing) {
    push("bad", false);
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, "stop"));
    push("idle", true);
    uint8_t buf[300];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(owner.vfs, rxFd, buf, sizeof(buf)));
    assertStatus(&owner, "rx_frames=0\n");
}

TEST(lora_rx, FullQueueDropsTheOldestAndCountsIt) {
    char p[8];
    for (int i = 0; i < CONFIG_WANTED_LORA_QUEUE_LEN + 3; i++) {
        snprintf(p, sizeof(p), "f%02d", i);
        push(p, true);
    }
    assertStatus(&owner, "rx_dropped=3\n");
    uint8_t buf[LORA_RECORD_HEADER + 3];
    TEST_ASSERT_EQUAL_INT((int)sizeof(buf),
                          VfsRead(owner.vfs, rxFd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(3, le32(buf + 8));
    TEST_ASSERT_EQUAL_MEMORY("f03", buf + LORA_RECORD_HEADER, 3);
    TEST_ASSERT_EQUAL_INT((int)sizeof(buf),
                          VfsRead(owner.vfs, rxFd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(0, le32(buf + 8));
}

TEST(lora_rx, PollReportsAQueuedRecord) {
    uint32_t avail = 0;
    TEST_ASSERT_EQUAL_INT(0, owner.drv->Poll(owner.drv->ctx, 1, &avail));
    push("xy", true);
    TEST_ASSERT_EQUAL_INT(VFS_POLL_IN,
                          owner.drv->Poll(owner.drv->ctx, 1, &avail));
    TEST_ASSERT_EQUAL_UINT32(LORA_RECORD_HEADER + 2, avail);
}

TEST_GROUP_RUNNER(lora_rx) {
    RUN_TEST_CASE(lora_rx, RecordCarriesTheHeaderAndPayload);
    RUN_TEST_CASE(lora_rx, ReadReturnsWholeRecordsOnly);
    RUN_TEST_CASE(lora_rx, BadCrcAndIdleRadioQueueNothing);
    RUN_TEST_CASE(lora_rx, FullQueueDropsTheOldestAndCountsIt);
    RUN_TEST_CASE(lora_rx, PollReportsAQueuedRecord);
}

/***************************************/
TEST_GROUP(lora_tx);

static int txFd;

static void setupTx(const char *setCmd) {
    TEST_ASSERT_TRUE(attach(&owner, GRANT_TX));
    ctl = openOn(&owner, "ctl", VFS_O_WRONLY);
    txFd = openOn(&owner, "tx", VFS_O_WRONLY);
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, setCmd));
    fake.log[0] = '\0';
}

TEST_SETUP(lora_tx) {
    reset();
    registerMain(&fullOps, true);
    setupTx(EU_SET);
}

TEST_TEAR_DOWN(lora_tx) {
    detach(&owner);
    LoraDevicesReset();
}

TEST(lora_tx, OneWriteIsOneFrame) {
    TEST_ASSERT_EQUAL_INT(5, writeFd(&owner, txFd, "hello"));
    TEST_ASSERT_EQUAL_STRING("transmit ", fake.log);
    TEST_ASSERT_EQUAL_MEMORY("hello", fake.sent, 5);
    assertStatus(&owner, "tx_frames=1\n");
    assertStatus(&owner, "state=standby\n");
    uint8_t big[LORA_PAYLOAD_MAX + 1] = {0};
    TEST_ASSERT_EQUAL_INT(-EMSGSIZE,
                          VfsWrite(owner.vfs, txFd, big, sizeof(big)));
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsWrite(owner.vfs, txFd, big, 0));
}

TEST(lora_tx, ReceiveResumesAfterATransmit) {
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, "start"));
    fake.log[0] = '\0';
    TEST_ASSERT_EQUAL_INT(2, writeFd(&owner, txFd, "hi"));
    TEST_ASSERT_EQUAL_STRING("stop transmit start ", fake.log);
    assertStatus(&owner, "state=rx\n");
}

TEST(lora_tx, BusyChannelRefusesWithoutACharge) {
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, EU_SET " cad=1"));
    TEST_ASSERT_GREATER_THAN_INT(0, writeFd(&owner, ctl, "start"));
    fake.busy = true;
    fake.log[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-EBUSY, writeFd(&owner, txFd, "hi"));
    TEST_ASSERT_EQUAL_STRING("stop cad start ", fake.log);
    assertStatus(&owner, "tx_busy=1\n");
    assertStatus(&owner, "tx_frames=0\n");
    assertStatus(&owner, "state=rx\n");
    fake.busy = false;
    fake.log[0] = '\0';
    TEST_ASSERT_EQUAL_INT(2, writeFd(&owner, txFd, "hi"));
    TEST_ASSERT_EQUAL_STRING("stop cad transmit start ", fake.log);
}

TEST(lora_tx, ChipFaultIsAnIoError) {
    fake.transmitRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-EIO, writeFd(&owner, txFd, "hi"));
    assertStatus(&owner, "tx_frames=0\n");
}

/* tx_ready_ms from the owner's status. */
static uint64_t readyMs(void) {
    char buf[512];
    status(&owner, buf, sizeof(buf));
    const char *p = strstr(buf, "tx_ready_ms=");
    TEST_ASSERT_NOT_NULL(p);
    unsigned long long v = 0;
    TEST_ASSERT_EQUAL_INT(1, sscanf(p, "tx_ready_ms=%llu", &v));
    return v;
}

/* 1 % of an hour is 36 s; a 255-byte SF12 frame at 125 kHz lasts 9.02 s, so
 * three fit and the fourth waits for the first to leave the hour. */
TEST(lora_tx, DutyCycleAccountRefusesAndSaysWhen) {
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=868100000 sf=12 bw=125"));
    uint8_t frame[LORA_PAYLOAD_MAX];
    memset(frame, 0xA5, sizeof(frame));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT((int)sizeof(frame),
                              VfsWrite(owner.vfs, txFd, frame, sizeof(frame)));
        DummyClockAdvance(10000 * MS);
    }
    TEST_ASSERT_EQUAL_INT(-EAGAIN,
                          VfsWrite(owner.vfs, txFd, frame, sizeof(frame)));
    /* The first frame leaves the hour 30 s before the 3600 s it stays. */
    uint64_t wait = readyMs();
    TEST_ASSERT_TRUE(wait > 3569000 && wait <= 3570000);
    DummyClockAdvance((wait - 1000) * MS);
    TEST_ASSERT_EQUAL_INT(-EAGAIN,
                          VfsWrite(owner.vfs, txFd, frame, sizeof(frame)));
    /* A short frame still fits now. */
    TEST_ASSERT_EQUAL_INT(5, writeFd(&owner, txFd, "short"));
    assertStatus(&owner, "tx_ready_ms=0\n");
    DummyClockAdvance(1000 * MS);
    TEST_ASSERT_EQUAL_INT((int)sizeof(frame),
                          VfsWrite(owner.vfs, txFd, frame, sizeof(frame)));
}

TEST(lora_tx, BandsKeepSeparateAccounts) {
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=868100000 sf=12 bw=125"));
    uint8_t frame[LORA_PAYLOAD_MAX] = {0};
    for (int i = 0; i < 3; i++)
        (void)VfsWrite(owner.vfs, txFd, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(-EAGAIN,
                          VfsWrite(owner.vfs, txFd, frame, sizeof(frame)));
    TEST_ASSERT_GREATER_THAN_INT(
        0, writeFd(&owner, ctl, "set freq=869525000 sf=12 bw=125"));
    TEST_ASSERT_EQUAL_INT((int)sizeof(frame),
                          VfsWrite(owner.vfs, txFd, frame, sizeof(frame)));
}

TEST(lora_tx, WithoutBandsOrTransmitNothingIsSent) {
    detach(&owner);
    LoraDevicesReset();
    registerMain(&fullOps, false);
    setupTx("set freq=915000000 sf=7 bw=125");
    TEST_ASSERT_EQUAL_INT(-EPERM, writeFd(&owner, txFd, "hi"));
    detach(&owner);
    LoraDevicesReset();
    registerMain(&rxOnlyOps, true);
    setupTx(EU_SET);
    TEST_ASSERT_EQUAL_INT(-ENOTSUP, writeFd(&owner, txFd, "hi"));
}

TEST_GROUP_RUNNER(lora_tx) {
    RUN_TEST_CASE(lora_tx, OneWriteIsOneFrame);
    RUN_TEST_CASE(lora_tx, ReceiveResumesAfterATransmit);
    RUN_TEST_CASE(lora_tx, BusyChannelRefusesWithoutACharge);
    RUN_TEST_CASE(lora_tx, ChipFaultIsAnIoError);
    RUN_TEST_CASE(lora_tx, DutyCycleAccountRefusesAndSaysWhen);
    RUN_TEST_CASE(lora_tx, BandsKeepSeparateAccounts);
    RUN_TEST_CASE(lora_tx, WithoutBandsOrTransmitNothingIsSent);
}

/***************************************/
TEST_GROUP(lora_airtime);

TEST_SETUP(lora_airtime) {}
TEST_TEAR_DOWN(lora_airtime) {}

/* SX126x data sheet 6.1.4, in the integer arithmetic RadioLib uses. */
TEST(lora_airtime, MatchesTheDataSheet) {
    lora_params_t lf = {869525000, 250000, 11, 5, 0x2B, 14, 16};
    TEST_ASSERT_EQUAL_UINT32(354304, LoraTimeOnAirUs(&lf, 16));
    TEST_ASSERT_EQUAL_UINT32(2156544, LoraTimeOnAirUs(&lf, 255));
    TEST_ASSERT_FALSE(LoraLowDataRate(&lf));
    lora_params_t ls = {869525000, 125000, 12, 8, 0x2B, 14, 16};
    TEST_ASSERT_TRUE(LoraLowDataRate(&ls));
    TEST_ASSERT_EQUAL_UINT32(3547136, LoraTimeOnAirUs(&ls, 50));
    lora_params_t sf = {869525000, 250000, 7, 5, 0x2B, 14, 16};
    TEST_ASSERT_EQUAL_UINT32(52864, LoraTimeOnAirUs(&sf, 50));
}

TEST_GROUP_RUNNER(lora_airtime) {
    RUN_TEST_CASE(lora_airtime, MatchesTheDataSheet);
}
