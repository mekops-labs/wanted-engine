/* SPDX-License-Identifier: Apache-2.0 */

/* devcheck — end-to-end check of the engine offload devices from inside WASM.
 *
 * Exercises /dev/sha256, /dev/inflate and /dev/ed25519 over the ordinary WASI
 * open/write/read path with known-answer vectors, printing TAP to the log
 * console. This is the wasm-wapp -> WASI fd_read/fd_write -> engine VFS ->
 * driver round trip that neither the engine's C unit tests (which call the VFS
 * API directly) nor Sheriff's host tests (which use a std fallback) cover.
 *
 * Run as the supervisor with the three devices granted (see
 * test/devcheck-config.json). Exits 0 iff every required check passes; the
 * runner asserts on the "ok"/"not ok" TAP lines. Ed25519 needs a platform
 * crypto backend (OpenSSL on Linux/SECURE_SOCKETS); without one the read
 * reports -ENOSYS, which still proves the transport, so it is a skip, not a
 * failure. */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void say(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

/* Drain a device read into buf, retrying on EAGAIN (a C wapp, unlike Zig's std,
 * can observe EAGAIN without trapping). Stops at EOF (0), a hard error, or full.
 * Returns bytes read. */
static int drain(int fd, char *buf, int cap) {
    int n = 0;
    int spins = 0;
    while (n < cap) {
        int r = read(fd, buf + n, cap - n);
        if (r > 0) {
            n += r;
            continue;
        }
        if (r == 0)
            break; /* EOF */
        if (errno == EAGAIN && spins++ < 1000)
            continue; /* nothing decoded yet — retry */
        break;        /* hard error */
    }
    return n;
}

/* SHA-256("abc") — FIPS 180-4 vector. */
static int check_sha256(void) {
    int fd = open("/dev/sha256", O_RDWR);
    if (fd < 0) {
        say("not ok 1 sha256 open\n");
        return 1;
    }
    if (write(fd, "abc", 3) != 3) {
        say("not ok 1 sha256 write\n");
        close(fd);
        return 1;
    }
    char hex[64];
    int n = drain(fd, hex, sizeof hex);
    close(fd);
    static const char want[64] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    if (n == 64 && memcmp(hex, want, 64) == 0) {
        say("ok 1 sha256\n");
        return 0;
    }
    say("not ok 1 sha256 mismatch\n");
    return 1;
}

/* gzip of "hello world\n" (printf 'hello world\n' | gzip -n). */
static const unsigned char GZ_HELLO[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xcb,
    0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
    0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0x0c, 0x00, 0x00, 0x00};

static int check_inflate(void) {
    int fd = open("/dev/inflate", O_RDWR);
    if (fd < 0) {
        say("not ok 2 inflate open\n");
        return 1;
    }
    /* 4-byte LE declared member length, then the member. */
    unsigned int len = (unsigned int)sizeof(GZ_HELLO);
    unsigned char prefix[4] = {(unsigned char)(len), (unsigned char)(len >> 8),
                               (unsigned char)(len >> 16),
                               (unsigned char)(len >> 24)};
    if (write(fd, prefix, 4) != 4) {
        say("not ok 2 inflate prefix\n");
        close(fd);
        return 1;
    }
    /* Feed the member, draining on short writes so a full output buffer never
     * wedges the pump (EAGAIN-free discipline). */
    size_t off = 0;
    char sink[64];
    while (off < sizeof(GZ_HELLO)) {
        int w = write(fd, GZ_HELLO + off, (int)(sizeof(GZ_HELLO) - off));
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (errno == EAGAIN) {
            (void)read(fd, sink, sizeof sink); /* free output space, retry */
            continue;
        }
        say("not ok 2 inflate write\n");
        close(fd);
        return 1;
    }
    char out[64];
    int n = drain(fd, out, sizeof out);
    close(fd);
    if (n == 12 && memcmp(out, "hello world\n", 12) == 0) {
        say("ok 2 inflate\n");
        return 0;
    }
    say("not ok 2 inflate mismatch\n");
    return 1;
}

/* RFC 8032 Ed25519 TEST 2: 1-byte message 0x72. */
static const unsigned char ED_PUB[32] = {
    0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a,
    0xa7, 0x4d, 0x1b, 0x7e, 0xbc, 0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4,
    0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c};
static const unsigned char ED_SIG[64] = {
    0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82,
    0x0b, 0x5f, 0x64, 0x25, 0x40, 0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50,
    0x3f, 0x8f, 0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda, 0x08,
    0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e, 0x45, 0x8f, 0x36, 0x13,
    0xd0, 0xf1, 0x1d, 0x8c, 0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a,
    0xee, 0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00};
static const unsigned char ED_MSG[1] = {0x72};

static int check_ed25519(void) {
    int fd = open("/dev/ed25519", O_RDWR);
    if (fd < 0) {
        say("not ok 3 ed25519 open\n");
        return 1;
    }
    /* pubkey(32) || sig(64) || message. */
    if (write(fd, ED_PUB, 32) != 32 || write(fd, ED_SIG, 64) != 64 ||
        write(fd, ED_MSG, 1) != 1) {
        say("not ok 3 ed25519 write\n");
        close(fd);
        return 1;
    }
    char verdict[8];
    int n = (int)read(fd, verdict, sizeof verdict);
    int e = errno;
    close(fd);
    if (n >= 2 && memcmp(verdict, "ok", 2) == 0) {
        say("ok 3 ed25519 verified\n");
        return 0;
    }
    if (n < 0 && e == ENOSYS) {
        /* No platform crypto backend in this build — transport proven, verdict
         * unavailable. Skip, not fail. */
        say("ok 3 ed25519 # SKIP no crypto backend (-ENOSYS)\n");
        return 0;
    }
    say("not ok 3 ed25519 verify\n");
    return 1;
}

/* Stop the engine so this runs exactly once (the engine otherwise respawns the
 * supervisor on exit). Needs the `wanted` control-plane grant. */
static void poweroff(void) {
    int fd = open("/dev/wanted/ctl", O_WRONLY);
    if (fd >= 0) {
        write(fd, "poweroff", 8);
        close(fd);
    }
}

int main(void) {
    say("TAP version 13\n1..3\n");
    int rc = 0;
    rc |= check_sha256();
    rc |= check_inflate();
    rc |= check_ed25519();
    say(rc == 0 ? "# devcheck: all round trips ok\n"
                : "# devcheck: FAILURES\n");
    poweroff();
    return rc;
}
