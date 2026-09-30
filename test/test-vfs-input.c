/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <platform.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-input.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-vfs-api.h>
#include <wasi/wasi-internal.h>
#include <wasi/wasi_types.h>

/* /dev/input: the per-device subtree, the grant grammar, and the read,
 * overflow and ownership contract. Devices are virtual: the test registers
 * them and pushes events, so no hardware is involved. */

#define KEY_A 30
#define GRANT_MAIN "devices=main,keymap=default"
#define GRANT_BOTH "devices=main,aux,keymap=default"

typedef struct peer_t {
    vfs_ctx_t vfs;
    vfs_driver_t *drv;
} peer_t;

static peer_t owner;
static peer_t injector;
static input_device_t *mainDev;
static input_device_t *auxDev;

static void registerDevices(void) {
    input_device_desc_t main = {
        "main", INPUT_TYPE_KEY | INPUT_TYPE_TEXT | INPUT_TYPE_REL, "default"};
    input_device_desc_t aux = {"aux", INPUT_TYPE_KEY, "default"};
    TEST_ASSERT_EQUAL_INT(0, InputDeviceRegister(&main, &mainDev));
    TEST_ASSERT_EQUAL_INT(0, InputDeviceRegister(&aux, &auxDev));
}

/* Install a driver with `options` and mount it. Returns false when the grant
 * was rejected, which is a failed launch. */
static bool attach(peer_t *p, const char *options) {
    p->vfs = VfsInit();
    p->drv = VfsInputInit(NULL, options);
    if (p->drv == NULL)
        return false;
    DevFs_Register(p->vfs, "input", p->drv);
    return true;
}

static void detach(peer_t *p) {
    VfsDestroy(&p->vfs);
    p->drv = NULL;
}

static void teardown(void) {
    detach(&injector);
    detach(&owner);
    InputDevicesReset();
}

static void push(input_device_t *dev, uint8_t sync, uint8_t type, uint16_t code,
                 int32_t value) {
    wanted_input_event_t ev = {sync, type, code, value};
    InputDevicePush(dev, &ev);
}

static int openNode(const char *path, int flags) {
    return VfsOpen(owner.vfs, path, flags);
}

/* Read one node into `buf`, NUL-terminated. Returns the byte count or -errno.
 */
static int readNode(const char *path, char *buf, size_t bufLen) {
    int fd = openNode(path, VFS_O_RDONLY);
    if (fd < 0)
        return fd;
    int n = VfsRead(owner.vfs, fd, buf, bufLen - 1);
    VfsClose(owner.vfs, fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static int writeVia(peer_t *p, const char *path, const void *payload,
                    size_t len) {
    int fd = VfsOpen(p->vfs, path, VFS_O_WRONLY);
    if (fd < 0)
        return fd;
    int n = VfsWrite(p->vfs, fd, payload, len);
    VfsClose(p->vfs, fd);
    return n;
}

static int writeNode(const char *path, const void *payload, size_t len) {
    return writeVia(&owner, path, payload, len);
}

/* The names in a directory, joined by ',' into `out`. Returns 0 or -errno. */
static int listDirVia(peer_t *p, const char *path, char *out, size_t outLen) {
    int fd = VfsOpen(p->vfs, path, VFS_O_RDONLY);
    if (fd < 0)
        return fd;

    uint8_t buf[512];
    uint64_t cookie = 0;
    size_t used = 0;
    int rc = VfsReadDir(p->vfs, fd, buf, sizeof(buf), &cookie, &used);
    VfsClose(p->vfs, fd);
    if (rc < 0)
        return rc;

    out[0] = '\0';
    size_t off = 0;
    while (off + sizeof(vfs_dirent_t) <= used) {
        vfs_dirent_t ent;
        memcpy(&ent, buf + off, sizeof(ent));
        off += sizeof(ent);
        if (out[0] != '\0')
            strncat(out, ",", outLen - strlen(out) - 1);
        strncat(out, (const char *)buf + off,
                (ent.d_namlen < outLen - strlen(out) - 1)
                    ? ent.d_namlen
                    : outLen - strlen(out) - 1);
        off += ent.d_namlen;
    }
    return 0;
}

static int listDir(const char *path, char *out, size_t outLen) {
    return listDirVia(&owner, path, out, outLen);
}

/***************************************/
TEST_GROUP(input_grant);
/***************************************/

TEST_SETUP(input_grant) { registerDevices(); }

TEST_TEAR_DOWN(input_grant) { teardown(); }

TEST(input_grant, MissingDevicesClauseFailsLaunch) {
    TEST_ASSERT_NULL(VfsInputInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsInputInit(NULL, ""));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices="));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "device=main,keymap=default"));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "keymap=default"));
}

TEST(input_grant, MissingKeymapClauseFailsLaunch) {
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main"));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,keymap="));
}

TEST(input_grant, UnknownDeviceFailsLaunch) {
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=nope,keymap=default"));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,nope,keymap=default"));
}

TEST(input_grant, UnknownKeymapFailsLaunch) {
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,keymap=other"));
}

TEST(input_grant, RepeatedOrEmptyDeviceFailsLaunch) {
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,main,keymap=default"));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,,keymap=default"));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,keymap=default,"));
}

TEST(input_grant, ClausesInAnotherOrderFailLaunch) {
    TEST_ASSERT_NULL(VfsInputInit(NULL, "keymap=default,devices=main"));
}

TEST(input_grant, TwoDevicesLaunch) {
    TEST_ASSERT_TRUE(attach(&owner, GRANT_BOTH));
}

TEST_GROUP_RUNNER(input_grant) {
    RUN_TEST_CASE(input_grant, MissingDevicesClauseFailsLaunch);
    RUN_TEST_CASE(input_grant, MissingKeymapClauseFailsLaunch);
    RUN_TEST_CASE(input_grant, UnknownDeviceFailsLaunch);
    RUN_TEST_CASE(input_grant, UnknownKeymapFailsLaunch);
    RUN_TEST_CASE(input_grant, RepeatedOrEmptyDeviceFailsLaunch);
    RUN_TEST_CASE(input_grant, ClausesInAnotherOrderFailLaunch);
    RUN_TEST_CASE(input_grant, TwoDevicesLaunch);
}

/***************************************/
TEST_GROUP(input_table);
/***************************************/

TEST_SETUP(input_table) {}

TEST_TEAR_DOWN(input_table) { InputDevicesReset(); }

TEST(input_table, IdenticalDeviceRegistersAgain) {
    input_device_desc_t d = {"main", INPUT_TYPE_KEY, "default"};
    input_device_t *first = NULL;
    input_device_t *again = NULL;
    TEST_ASSERT_EQUAL_INT(0, InputDeviceRegister(&d, &first));
    TEST_ASSERT_EQUAL_INT(0, InputDeviceRegister(&d, &again));
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_TRUE(first == again);
}

TEST(input_table, SameNameWithOtherPropertiesIsEexist) {
    input_device_desc_t d = {"main", INPUT_TYPE_KEY, "default"};
    TEST_ASSERT_EQUAL_INT(0, InputDeviceRegister(&d, NULL));
    d.types = INPUT_TYPE_KEY | INPUT_TYPE_TEXT;
    TEST_ASSERT_EQUAL_INT(-EEXIST, InputDeviceRegister(&d, NULL));
    d.types = INPUT_TYPE_KEY;
    d.keymap = "other";
    TEST_ASSERT_EQUAL_INT(-EEXIST, InputDeviceRegister(&d, NULL));
}

TEST(input_table, BadNamesTypesAndKeymapsAreEinval) {
    input_device_desc_t d = {"main", INPUT_TYPE_KEY, "default"};

    d.name = NULL;
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));
    d.name = "";
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));
    d.name = "has space";
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));
    d.name = "sixteen_chars_xxx";
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));

    d.name = "main";
    d.types = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));
    d.types = 0x8;
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));

    d.types = INPUT_TYPE_KEY;
    d.keymap = NULL;
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));
    d.keymap = "";
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));
    d.keymap = "bad,map";
    TEST_ASSERT_EQUAL_INT(-EINVAL, InputDeviceRegister(&d, NULL));
}

TEST(input_table, TableFullIsEnospc) {
    char name[INPUT_NAME_MAX + 1];
    input_device_desc_t d = {name, INPUT_TYPE_KEY, "default"};

    for (int i = 0; i < CONFIG_WANTED_INPUT_MAX_DEVICES; i++) {
        snprintf(name, sizeof(name), "dev%d", i);
        TEST_ASSERT_EQUAL_INT(0, InputDeviceRegister(&d, NULL));
    }
    snprintf(name, sizeof(name), "extra");
    TEST_ASSERT_EQUAL_INT(-ENOSPC, InputDeviceRegister(&d, NULL));
}

TEST_GROUP_RUNNER(input_table) {
    RUN_TEST_CASE(input_table, IdenticalDeviceRegistersAgain);
    RUN_TEST_CASE(input_table, SameNameWithOtherPropertiesIsEexist);
    RUN_TEST_CASE(input_table, BadNamesTypesAndKeymapsAreEinval);
    RUN_TEST_CASE(input_table, TableFullIsEnospc);
}

/***************************************/
TEST_GROUP(input_tree);
/***************************************/

TEST_SETUP(input_tree) {
    registerDevices();
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
}

TEST_TEAR_DOWN(input_tree) { teardown(); }

TEST(input_tree, InfoListsTheTypesAndTheKeymap) {
    char buf[64];
    TEST_ASSERT_TRUE(readNode("/dev/input/main/info", buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_STRING("key text rel keymap=default\n", buf);
}

TEST(input_tree, InfoOfAKeyOnlyDeviceListsKeyOnly) {
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, "devices=aux,keymap=default"));
    char buf[64];
    TEST_ASSERT_TRUE(readNode("/dev/input/aux/info", buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_STRING("key keymap=default\n", buf);
}

TEST(input_tree, UngrantedDeviceIsEnoent) {
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/input/aux/info", buf, 8));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/input/aux/events", buf, 8));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/input/aux", buf, 8));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/input/nope/info", buf, 8));
}

TEST(input_tree, UnknownNodeIsEnoent) {
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/input/main/ctl", buf, 8));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/input/main/inject", buf, 8));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/input/main/x", buf, 8));
}

TEST(input_tree, RootListsExactlyTheGrantedDevices) {
    char names[64];
    TEST_ASSERT_EQUAL_INT(0, listDir("/dev/input", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("main", names);

    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_BOTH));
    TEST_ASSERT_EQUAL_INT(0, listDir("/dev/input", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("main,aux", names);
}

TEST(input_tree, DeviceListsEventsAndInfo) {
    char names[64];
    TEST_ASSERT_EQUAL_INT(0, listDir("/dev/input/main", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("events,info", names);
}

TEST(input_tree, InfoAndEventsRefuseWrites) {
    wanted_input_event_t ev = {1, EV_KEY, KEY_A, 1};
    TEST_ASSERT_EQUAL_INT(-EPERM, writeNode("/dev/input/main/info", "x", 1));
    TEST_ASSERT_EQUAL_INT(-EPERM,
                          writeNode("/dev/input/main/events", &ev, sizeof(ev)));
}

TEST(input_tree, DirectoriesRefuseReadsAndWrites) {
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EISDIR, readNode("/dev/input", buf, 8));
    TEST_ASSERT_EQUAL_INT(-EISDIR, readNode("/dev/input/main", buf, 8));
    TEST_ASSERT_EQUAL_INT(-EISDIR, writeNode("/dev/input/main", "x", 1));
}

TEST_GROUP_RUNNER(input_tree) {
    RUN_TEST_CASE(input_tree, InfoListsTheTypesAndTheKeymap);
    RUN_TEST_CASE(input_tree, InfoOfAKeyOnlyDeviceListsKeyOnly);
    RUN_TEST_CASE(input_tree, UngrantedDeviceIsEnoent);
    RUN_TEST_CASE(input_tree, UnknownNodeIsEnoent);
    RUN_TEST_CASE(input_tree, RootListsExactlyTheGrantedDevices);
    RUN_TEST_CASE(input_tree, DeviceListsEventsAndInfo);
    RUN_TEST_CASE(input_tree, InfoAndEventsRefuseWrites);
    RUN_TEST_CASE(input_tree, DirectoriesRefuseReadsAndWrites);
}

/***************************************/
TEST_GROUP(input_events);
/***************************************/

static int eventsFd;

TEST_SETUP(input_events) {
    registerDevices();
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
    eventsFd =
        openNode("/dev/input/main/events", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(eventsFd >= 0);
}

TEST_TEAR_DOWN(input_events) { teardown(); }

TEST(input_events, RecordIsEightBytesLittleEndian) {
    uint8_t rec[INPUT_EVENT_BYTES];
    push(mainDev, 1, EV_KEY, 0x0161, -2);

    TEST_ASSERT_EQUAL_INT(8, VfsRead(owner.vfs, eventsFd, rec, sizeof(rec)));
    const uint8_t want[8] = {0x01, 0x01, 0x61, 0x01, 0xFE, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_MEMORY(want, rec, 8);
}

TEST(input_events, SmallBufferIsEinval) {
    uint8_t rec[INPUT_EVENT_BYTES];
    push(mainDev, 1, EV_KEY, KEY_A, 1);

    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsRead(owner.vfs, eventsFd, rec, 7));
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsRead(owner.vfs, eventsFd, rec, 0));
    TEST_ASSERT_EQUAL_INT(8, VfsRead(owner.vfs, eventsFd, rec, sizeof(rec)));
}

TEST(input_events, ReadNeverReturnsPartOfARecord) {
    uint8_t buf[12];
    push(mainDev, 0, EV_KEY, KEY_A, 1);
    push(mainDev, 1, EV_TEXT, 0, 'a');

    TEST_ASSERT_EQUAL_INT(8, VfsRead(owner.vfs, eventsFd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT8(EV_KEY, buf[1]);
    TEST_ASSERT_EQUAL_INT(8, VfsRead(owner.vfs, eventsFd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT8(EV_TEXT, buf[1]);
}

TEST(input_events, RecordsArriveInOrderWithTheirBatchMarker) {
    uint8_t buf[INPUT_EVENT_BYTES * 4];
    push(mainDev, 0, EV_KEY, KEY_A, 1);
    push(mainDev, 0, EV_TEXT, 0, 'a');
    push(mainDev, 1, EV_REL, 8, 1);

    TEST_ASSERT_EQUAL_INT(24, VfsRead(owner.vfs, eventsFd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(EV_KEY, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[8]);
    TEST_ASSERT_EQUAL_UINT8(EV_TEXT, buf[9]);
    TEST_ASSERT_EQUAL_UINT8(1, buf[16]);
    TEST_ASSERT_EQUAL_UINT8(EV_REL, buf[17]);
}

TEST(input_events, NonBlockingReadOfAnEmptyQueueIsEagain) {
    uint8_t rec[INPUT_EVENT_BYTES];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(owner.vfs, eventsFd, rec, 8));
}

TEST(input_events, ReadConsumesTheRecord) {
    uint8_t rec[INPUT_EVENT_BYTES];
    push(mainDev, 1, EV_KEY, KEY_A, 1);
    TEST_ASSERT_EQUAL_INT(8, VfsRead(owner.vfs, eventsFd, rec, 8));
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(owner.vfs, eventsFd, rec, 8));
}

TEST(input_events, EventsAreNotSeekable) {
    long pos;
    TEST_ASSERT_EQUAL_INT(-ESPIPE,
                          VfsSeek(owner.vfs, eventsFd, 0, VFS_SEEK_SET, &pos));
}

TEST_GROUP_RUNNER(input_events) {
    RUN_TEST_CASE(input_events, RecordIsEightBytesLittleEndian);
    RUN_TEST_CASE(input_events, SmallBufferIsEinval);
    RUN_TEST_CASE(input_events, ReadNeverReturnsPartOfARecord);
    RUN_TEST_CASE(input_events, RecordsArriveInOrderWithTheirBatchMarker);
    RUN_TEST_CASE(input_events, NonBlockingReadOfAnEmptyQueueIsEagain);
    RUN_TEST_CASE(input_events, ReadConsumesTheRecord);
    RUN_TEST_CASE(input_events, EventsAreNotSeekable);
}

/***************************************/
TEST_GROUP(input_overflow);
/***************************************/

TEST_SETUP(input_overflow) {
    registerDevices();
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
    eventsFd =
        openNode("/dev/input/main/events", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(eventsFd >= 0);
}

TEST_TEAR_DOWN(input_overflow) { teardown(); }

/* Drain the queue; returns the record count. */
static int drain(uint8_t *out, size_t outLen) {
    int total = 0;
    for (;;) {
        int n = VfsRead(owner.vfs, eventsFd, out + total, outLen - total);
        if (n <= 0)
            return total / INPUT_EVENT_BYTES;
        total += n;
    }
}

TEST(input_overflow, ExactlyFullQueueKeepsEveryRecord) {
    static uint8_t buf[INPUT_QUEUE_LEN * INPUT_EVENT_BYTES];
    for (int i = 0; i < INPUT_QUEUE_LEN; i++)
        push(mainDev, 1, EV_KEY, KEY_A, i & 1);

    TEST_ASSERT_EQUAL_INT(INPUT_QUEUE_LEN, drain(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT8(EV_KEY, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(EV_KEY, buf[sizeof(buf) - 7]);
}

TEST(input_overflow, OverflowLeavesOneSynDropped) {
    static uint8_t buf[INPUT_QUEUE_LEN * INPUT_EVENT_BYTES];
    for (int i = 0; i < INPUT_QUEUE_LEN + 1; i++)
        push(mainDev, 1, EV_KEY, KEY_A, 1);

    TEST_ASSERT_EQUAL_INT(1, drain(buf, sizeof(buf)));
    const uint8_t want[8] = {1, EV_SYN, SYN_DROPPED, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(want, buf, 8);
}

TEST(input_overflow, RecordsAfterTheOverflowAreKept) {
    static uint8_t buf[INPUT_QUEUE_LEN * INPUT_EVENT_BYTES];
    for (int i = 0; i < INPUT_QUEUE_LEN + 1; i++)
        push(mainDev, 1, EV_KEY, KEY_A, 1);
    push(mainDev, 1, EV_TEXT, 0, 'z');

    TEST_ASSERT_EQUAL_INT(2, drain(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT8(EV_SYN, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(EV_TEXT, buf[9]);
}

TEST(input_overflow, ADrainedQueueOverflowsAgain) {
    static uint8_t buf[INPUT_QUEUE_LEN * INPUT_EVENT_BYTES];
    for (int i = 0; i < INPUT_QUEUE_LEN + 1; i++)
        push(mainDev, 1, EV_KEY, KEY_A, 1);
    TEST_ASSERT_EQUAL_INT(1, drain(buf, sizeof(buf)));

    for (int i = 0; i < INPUT_QUEUE_LEN + 1; i++)
        push(mainDev, 1, EV_KEY, KEY_A, 1);
    TEST_ASSERT_EQUAL_INT(1, drain(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT8(EV_SYN, buf[1]);
}

TEST_GROUP_RUNNER(input_overflow) {
    RUN_TEST_CASE(input_overflow, ExactlyFullQueueKeepsEveryRecord);
    RUN_TEST_CASE(input_overflow, OverflowLeavesOneSynDropped);
    RUN_TEST_CASE(input_overflow, RecordsAfterTheOverflowAreKept);
    RUN_TEST_CASE(input_overflow, ADrainedQueueOverflowsAgain);
}

/***************************************/
TEST_GROUP(input_owner);
/***************************************/

TEST_SETUP(input_owner) { registerDevices(); }

TEST_TEAR_DOWN(input_owner) { teardown(); }

TEST(input_owner, SecondGrantOfTheSameDeviceFailsLaunch) {
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
    TEST_ASSERT_NULL(VfsInputInit(NULL, GRANT_MAIN));
    TEST_ASSERT_NULL(VfsInputInit(NULL, GRANT_BOTH));
}

TEST(input_owner, RefusedGrantClaimsNothing) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=aux,keymap=default"));
    TEST_ASSERT_NULL(VfsInputInit(NULL, GRANT_BOTH));

    vfs_driver_t *m = VfsInputInit(NULL, GRANT_MAIN);
    TEST_ASSERT_NOT_NULL(m);
    m->Destroy(m);
}

TEST(input_owner, DestroyReleasesTheDevice) {
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
    detach(&owner);

    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
}

TEST(input_owner, OwnersOfDifferentDevicesCoexist) {
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
    vfs_driver_t *a = VfsInputInit(NULL, "devices=aux,keymap=default");
    TEST_ASSERT_NOT_NULL(a);
    a->Destroy(a);
}

TEST(input_owner, NewOwnerDoesNotSeeRecordsQueuedBeforeIt) {
    push(mainDev, 1, EV_KEY, KEY_A, 1);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));

    uint8_t rec[INPUT_EVENT_BYTES];
    int fd = openNode("/dev/input/main/events", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(owner.vfs, fd, rec, sizeof(rec)));
}

TEST(input_owner, DeviceStaysRegisteredAcrossOwners) {
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));

    push(mainDev, 1, EV_KEY, KEY_A, 1);
    uint8_t rec[INPUT_EVENT_BYTES];
    int fd = openNode("/dev/input/main/events", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_EQUAL_INT(8, VfsRead(owner.vfs, fd, rec, sizeof(rec)));
}

TEST_GROUP_RUNNER(input_owner) {
    RUN_TEST_CASE(input_owner, SecondGrantOfTheSameDeviceFailsLaunch);
    RUN_TEST_CASE(input_owner, RefusedGrantClaimsNothing);
    RUN_TEST_CASE(input_owner, DestroyReleasesTheDevice);
    RUN_TEST_CASE(input_owner, OwnersOfDifferentDevicesCoexist);
    RUN_TEST_CASE(input_owner, NewOwnerDoesNotSeeRecordsQueuedBeforeIt);
    RUN_TEST_CASE(input_owner, DeviceStaysRegisteredAcrossOwners);
}

#ifdef CONFIG_WANTED_VFS_INPUT_INJECT
/***************************************/
TEST_GROUP(input_inject);
/***************************************/

#define GRANT_INJECT "devices=main,keymap=default,inject"

static int ownerFd;

TEST_SETUP(input_inject) {
    registerDevices();
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
    TEST_ASSERT_TRUE(attach(&injector, GRANT_INJECT));
    ownerFd = openNode("/dev/input/main/events", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(ownerFd >= 0);
}

TEST_TEAR_DOWN(input_inject) { teardown(); }

TEST(input_inject, InjectGrantListsInfoAndInjectOnly) {
    char names[64];
    TEST_ASSERT_EQUAL_INT(
        0, listDirVia(&injector, "/dev/input/main", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("info,inject", names);
    TEST_ASSERT_EQUAL_INT(
        -ENOENT, VfsOpen(injector.vfs, "/dev/input/main/events", VFS_O_RDONLY));
}

TEST(input_inject, OwnerGrantHasNoInjectNode) {
    char names[64];
    TEST_ASSERT_EQUAL_INT(0, listDir("/dev/input/main", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("events,info", names);
    TEST_ASSERT_EQUAL_INT(
        -ENOENT, VfsOpen(owner.vfs, "/dev/input/main/inject", VFS_O_WRONLY));
}

TEST(input_inject, InjectedRecordsReachTheOwnerByteForByte) {
    const uint8_t batch[24] = {
        0x00, 0x01, 0x1E, 0x00, 0x01, 0x00, 0x00, 0x00, /* KEY_A down */
        0x00, 0xF0, 0x00, 0x00, 0x61, 0x00, 0x00, 0x00, /* text 'a' */
        0x01, 0x02, 0x08, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, /* wheel -1 */
    };
    uint8_t got[32];

    TEST_ASSERT_EQUAL_INT(24, writeVia(&injector, "/dev/input/main/inject",
                                       batch, sizeof(batch)));
    TEST_ASSERT_EQUAL_INT(24, VfsRead(owner.vfs, ownerFd, got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(batch, got, 24);
}

TEST(input_inject, PartialRecordIsEinvalAndQueuesNothing) {
    uint8_t bytes[16] = {0};
    uint8_t got[16];

    TEST_ASSERT_EQUAL_INT(
        -EINVAL, writeVia(&injector, "/dev/input/main/inject", bytes, 7));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, writeVia(&injector, "/dev/input/main/inject", bytes, 12));
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(owner.vfs, ownerFd, got, 16));
}

TEST(input_inject, InjectOverflowYieldsSynDropped) {
    static uint8_t batch[(INPUT_QUEUE_LEN + 1) * INPUT_EVENT_BYTES];
    uint8_t got[INPUT_QUEUE_LEN * INPUT_EVENT_BYTES];

    for (size_t i = 0; i < sizeof(batch); i += INPUT_EVENT_BYTES) {
        memset(batch + i, 0, INPUT_EVENT_BYTES);
        batch[i] = 1;
        batch[i + 1] = EV_KEY;
    }
    TEST_ASSERT_EQUAL_INT(
        (int)sizeof(batch),
        writeVia(&injector, "/dev/input/main/inject", batch, sizeof(batch)));
    TEST_ASSERT_EQUAL_INT(8, VfsRead(owner.vfs, ownerFd, got, sizeof(got)));
    TEST_ASSERT_EQUAL_UINT8(EV_SYN, got[1]);
    TEST_ASSERT_EQUAL_UINT8(SYN_DROPPED, got[2]);
}

TEST(input_inject, InjectNodeRefusesReads) {
    char buf[8];
    int fd = VfsOpen(injector.vfs, "/dev/input/main/inject", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(-EPERM, VfsRead(injector.vfs, fd, buf, sizeof(buf)));
}

TEST(input_inject, InjectGrantDoesNotClaimTheDevice) {
    peer_t second;
    TEST_ASSERT_TRUE(attach(&second, GRANT_INJECT));
    detach(&second);
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
}

TEST(input_inject, InjectGrantMayLaunchBeforeTheOwner) {
    detach(&injector);
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&injector, GRANT_INJECT));
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
}

TEST(input_inject, MisplacedInjectTokenFailsLaunch) {
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,inject,keymap=default"));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,keymap=default,inject,"));
    TEST_ASSERT_NULL(
        VfsInputInit(NULL, "devices=main,keymap=default,inject,inject"));
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,keymap=default,other"));
}

TEST_GROUP_RUNNER(input_inject) {
    RUN_TEST_CASE(input_inject, InjectGrantListsInfoAndInjectOnly);
    RUN_TEST_CASE(input_inject, OwnerGrantHasNoInjectNode);
    RUN_TEST_CASE(input_inject, InjectedRecordsReachTheOwnerByteForByte);
    RUN_TEST_CASE(input_inject, PartialRecordIsEinvalAndQueuesNothing);
    RUN_TEST_CASE(input_inject, InjectOverflowYieldsSynDropped);
    RUN_TEST_CASE(input_inject, InjectNodeRefusesReads);
    RUN_TEST_CASE(input_inject, InjectGrantDoesNotClaimTheDevice);
    RUN_TEST_CASE(input_inject, InjectGrantMayLaunchBeforeTheOwner);
    RUN_TEST_CASE(input_inject, MisplacedInjectTokenFailsLaunch);
}
#endif /* CONFIG_WANTED_VFS_INPUT_INJECT */

#ifndef CONFIG_WANTED_VFS_INPUT_INJECT
/***************************************/
TEST_GROUP(input_inject);
/***************************************/

TEST_SETUP(input_inject) { registerDevices(); }

TEST_TEAR_DOWN(input_inject) { teardown(); }

/* A build without injection never reads an inject grant as an owner's. */
TEST(input_inject, InjectGrantFailsLaunchWithoutTheBuildOption) {
    TEST_ASSERT_NULL(VfsInputInit(NULL, "devices=main,keymap=default,inject"));
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
}

TEST_GROUP_RUNNER(input_inject) {
    RUN_TEST_CASE(input_inject, InjectGrantFailsLaunchWithoutTheBuildOption);
}
#endif /* !CONFIG_WANTED_VFS_INPUT_INJECT */

/***************************************/
TEST_GROUP(input_poll);
/***************************************/

TEST_SETUP(input_poll) {
    registerDevices();
    TEST_ASSERT_TRUE(attach(&owner, GRANT_MAIN));
    eventsFd = openNode("/dev/input/main/events", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(eventsFd >= 0);
}

TEST_TEAR_DOWN(input_poll) { teardown(); }

static __wasi_subscription_t eventsSub(int fd) {
    __wasi_subscription_t s;
    memset(&s, 0, sizeof(s));
    s.userdata = 1;
    s.type = __WASI_EVENTTYPE_FD_READ;
    s.u.fd_readwrite.fd = (uint32_t)fd;
    return s;
}

static __wasi_subscription_t clockSub(uint64_t timeoutNs) {
    __wasi_subscription_t s;
    memset(&s, 0, sizeof(s));
    s.userdata = 2;
    s.type = __WASI_EVENTTYPE_CLOCK;
    s.u.clock.id = 1; /* monotonic */
    s.u.clock.timeout = timeoutNs;
    return s;
}

TEST(input_poll, EventsAreNotReadableWhileTheQueueIsEmpty) {
    uint32_t avail = 99;
    TEST_ASSERT_EQUAL_INT(0, VfsPoll(owner.vfs, eventsFd, &avail));
}

TEST(input_poll, EventsAreReadableWithTheQueuedByteCount) {
    uint32_t avail = 0;
    push(mainDev, 0, EV_KEY, KEY_A, 1);
    push(mainDev, 1, EV_TEXT, 0, 'a');

    TEST_ASSERT_EQUAL_INT(VFS_POLL_IN, VfsPoll(owner.vfs, eventsFd, &avail));
    TEST_ASSERT_EQUAL_UINT32(2 * INPUT_EVENT_BYTES, avail);
}

TEST(input_poll, EventsAreNotReadableOnceDrained) {
    uint8_t rec[INPUT_EVENT_BYTES];
    uint32_t avail = 0;
    push(mainDev, 1, EV_KEY, KEY_A, 1);
    TEST_ASSERT_EQUAL_INT(8, VfsRead(owner.vfs, eventsFd, rec, sizeof(rec)));

    TEST_ASSERT_EQUAL_INT(0, VfsPoll(owner.vfs, eventsFd, &avail));
}

TEST(input_poll, InfoIsAlwaysReadable) {
    uint32_t avail = 0;
    int fd = openNode("/dev/input/main/info", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_TRUE((VfsPoll(owner.vfs, fd, &avail) & VFS_POLL_IN) != 0);
}

/* One poll_oneoff waits on events beside another source and wakes on a
 * record. */
TEST(input_poll, PollOneoffWakesOnAQueuedRecord) {
    __wasi_subscription_t subs[2] = {eventsSub(eventsFd), clockSub(2000000)};
    __wasi_event_t ev[2];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(owner.vfs, subs, ev, 2, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT8(__WASI_EVENTTYPE_CLOCK, ev[0].type);

    push(mainDev, 1, EV_KEY, KEY_A, 1);
    subs[1] = clockSub(5000000000ULL);
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(owner.vfs, subs, ev, 2, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT8(__WASI_EVENTTYPE_FD_READ, ev[0].type);
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS, ev[0].error);
    TEST_ASSERT_EQUAL_UINT64(INPUT_EVENT_BYTES, ev[0].fd_readwrite.nbytes);
}

/* A stop raises the wake descriptor, which ends a blocked events read. */
TEST(input_poll, StopInterruptsABlockedEventsRead) {
    int wake = PlatformWakeCreate();
    TEST_ASSERT_TRUE(wake >= 0);
    owner.drv->SetWake(owner.drv->ctx, wake);
    VfsSetWakeFd(owner.vfs, wake);
    PlatformWakeRaise(wake);

    uint8_t rec[INPUT_EVENT_BYTES];
    TEST_ASSERT_EQUAL_INT(-EINTR,
                          VfsRead(owner.vfs, eventsFd, rec, sizeof(rec)));
}

TEST_GROUP_RUNNER(input_poll) {
    RUN_TEST_CASE(input_poll, EventsAreNotReadableWhileTheQueueIsEmpty);
    RUN_TEST_CASE(input_poll, EventsAreReadableWithTheQueuedByteCount);
    RUN_TEST_CASE(input_poll, EventsAreNotReadableOnceDrained);
    RUN_TEST_CASE(input_poll, InfoIsAlwaysReadable);
    RUN_TEST_CASE(input_poll, PollOneoffWakesOnAQueuedRecord);
    RUN_TEST_CASE(input_poll, StopInterruptsABlockedEventsRead);
}

/***************************************/
TEST_GROUP(input_config);
/***************************************/

#define INPUT_CFG(list) "{\"system\":{\"inputs\":" list "}}"

TEST_SETUP(input_config) {}

TEST_TEAR_DOWN(input_config) { teardown(); }

static int parseInputs(const char *json) {
    return WantedParseConfig(json, strlen(json));
}

TEST(input_config, SystemConfigDeclaresADevice) {
    TEST_ASSERT_EQUAL_INT(
        0, parseInputs(INPUT_CFG("[{\"name\":\"kbd\",\"types\":[\"key\","
                                 "\"text\",\"rel\"],\"keymap\":\"us\"}]")));
    TEST_ASSERT_TRUE(attach(&owner, "devices=kbd,keymap=us"));
    char buf[64];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/input/kbd/info", buf, 64));
    TEST_ASSERT_EQUAL_STRING("key text rel keymap=us\n", buf);
}

TEST(input_config, TypesAreAMaskWhateverTheirOrder) {
    TEST_ASSERT_EQUAL_INT(
        0, parseInputs(INPUT_CFG("[{\"name\":\"kbd\",\"types\":[\"rel\","
                                 "\"key\"],\"keymap\":\"us\"}]")));
    TEST_ASSERT_TRUE(attach(&owner, "devices=kbd,keymap=us"));
    char buf[64];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/input/kbd/info", buf, 64));
    TEST_ASSERT_EQUAL_STRING("key rel keymap=us\n", buf);
}

/* A declaration the engine cannot honour fails the parse; nothing is skipped.
 */
TEST(input_config, MalformedDeclarationFailsTheParse) {
    const char *bad[] = {
        INPUT_CFG("{}"),
        INPUT_CFG("[1]"),
        INPUT_CFG("[{\"types\":[\"key\"],\"keymap\":\"us\"}]"),
        INPUT_CFG("[{\"name\":\"a\",\"keymap\":\"us\"}]"),
        INPUT_CFG("[{\"name\":\"a\",\"types\":[\"key\"]}]"),
        INPUT_CFG("[{\"name\":\"a\",\"types\":[],\"keymap\":\"us\"}]"),
        INPUT_CFG("[{\"name\":\"a\",\"types\":\"key\",\"keymap\":\"us\"}]"),
        INPUT_CFG("[{\"name\":\"a\",\"types\":[\"abs\"],\"keymap\":\"us\"}]"),
        INPUT_CFG("[{\"name\":\"a\",\"types\":[1],\"keymap\":\"us\"}]"),
        INPUT_CFG("[{\"name\":\"a b\",\"types\":[\"key\"],\"keymap\":\"us\"}]"),
        INPUT_CFG("[{\"name\":\"a\",\"types\":[\"key\"],\"keymap\":\"u,s\"}]"),
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(parseInputs(bad[i]) != 0, bad[i]);
        InputDevicesReset();
    }
}

TEST(input_config, DeclaringTheSameDeviceAgainIsAccepted) {
    const char *cfg =
        INPUT_CFG("[{\"name\":\"kbd\",\"types\":[\"key\"],\"keymap\":\"us\"}]");
    TEST_ASSERT_EQUAL_INT(0, parseInputs(cfg));
    TEST_ASSERT_EQUAL_INT(0, parseInputs(cfg));
}

TEST(input_config, DeclaringADeviceDifferentlyFailsTheParse) {
    TEST_ASSERT_EQUAL_INT(
        0, parseInputs(INPUT_CFG("[{\"name\":\"kbd\",\"types\":[\"key\"],"
                                 "\"keymap\":\"us\"}]")));
    TEST_ASSERT_TRUE(
        parseInputs(INPUT_CFG("[{\"name\":\"kbd\",\"types\":[\"key\","
                              "\"text\"],\"keymap\":\"us\"}]")) != 0);
}

/* Naming input in a launch config reaches this driver: it is in the core
 * table. */
TEST(input_config, DriverIsListedAmongTheGrantableDrivers) {
    char names[512];
    TEST_ASSERT_GREATER_THAN_INT(0, WantedListDrivers(names, sizeof(names)));
    TEST_ASSERT_NOT_NULL(strstr(names, "input"));
}

TEST_GROUP_RUNNER(input_config) {
    RUN_TEST_CASE(input_config, DriverIsListedAmongTheGrantableDrivers);
    RUN_TEST_CASE(input_config, SystemConfigDeclaresADevice);
    RUN_TEST_CASE(input_config, TypesAreAMaskWhateverTheirOrder);
    RUN_TEST_CASE(input_config, MalformedDeclarationFailsTheParse);
    RUN_TEST_CASE(input_config, DeclaringTheSameDeviceAgainIsAccepted);
    RUN_TEST_CASE(input_config, DeclaringADeviceDifferentlyFailsTheParse);
}
