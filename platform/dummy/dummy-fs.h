/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wanted-api.h>

/* Reset all in-memory file/directory/fd state. Call in TEST_SETUP. */
void DummyFsReset(void);

/* Reset the monotonic clock counter to zero and reseed the PRNG. */
void DummyClockReset(void);

/* Advance the monotonic clock by `ns` nanoseconds without sleeping. */
void DummyClockAdvance(uint64_t ns);

/* Empty the in-memory registry. Call in TEST_SETUP. */
void DummyRegistryReset(void);

/* Populate the registry from `entries`, upserting by name. Returns the number
 * stored, or -ENOSPC when the table is full. The test write path, since the
 * dummy platform cannot stream an image to a host file. */
int DummyRegistrySeed(const reg_entry_t *entries, size_t count);

/* ── GPIO fake control (dummy-gpio.c) ───────────────────────────────────── */

/* Release every fake GPIO line. Call in TEST_SETUP. */
void DummyGpioReset(void);

/* Drive the fake line at `address`, so a test can present an input level.
 * Returns -ENOENT when no grant opened that address. */
int DummyGpioSetLevel(const char *address, bool level);

/* Read the fake line at `address`, so a test can observe what a wapp drove.
 * Returns -ENOENT when no grant opened that address. */
int DummyGpioGetLevel(const char *address, bool *level);

/* ── OTA A/B fake control (dummy-ota.c) ──────────────────────────────────── */

/* Reset to a confirmed slot 'a' with nothing staged. Call in TEST_SETUP. */
void DummyOtaReset(void);

/* The slot the fake is running from. */
char DummyOtaActiveSlot(void);

/* Bytes committed to the staged slot, or -1 when nothing is staged. */
int DummyOtaStagedLen(void);

/* ── UART loopback fake control (dummy-uart.c) ───────────────────────────── */

/* Release the fake port and empty its ring. Call in TEST_SETUP. */
void DummyUartReset(void);

/* Report the line settings the driver last applied. Returns -ENOENT when no
 * grant opened the port. Pass NULL for a field a test does not check. */
int DummyUartGetLine(uint32_t *baud, uint8_t *databits, uint8_t *parity,
                     uint8_t *stopbits);

/* Bytes waiting in the loopback ring, or -ENOENT when the port is closed. */
int DummyUartRxLen(void);

/* ── Wapp runtime-state mock control (dummy-wapps.c) ────────────────────── */

/* Clear the in-memory wapp runtime-state table. Call in TEST_SETUP. */
void DummyWappStateReset(void);

/* Populate the wapp runtime-state table from `states`, upserting by name.
 * Returns the number stored, or -ENOSPC when the table is full. Puts a wapp
 * into a known status without a real WASM runtime. */
int DummyWappStateSeed(const wapp_state_t *states, size_t count);

/* ── Network mock control (dummy-net.c) ─────────────────────────────────── */

/* Reset socket pool, buffers, and controllable results. Call in TEST_SETUP. */
void DummyNetReset(void);

/* Make the next PlatformNetOpen return NULL (simulate open failure). */
void DummyNetSetOpenFail(int fail);

/* Set the value PlatformNetConnect returns (0 = success, <0 = failure). */
void DummyNetSetConnectResult(int result);

/* Set the value PlatformNetListen returns (0 = success, <0 = failure). */
void DummyNetSetListenResult(int result);

/* Set the value PlatformNetAccept returns; 0 hands out a fresh socket. */
void DummyNetSetAcceptResult(int result);

/* Seed bytes that PlatformNetRecv will return (drained across calls), and read
 * back what PlatformNetSend captured. Buffers are per socket: the plain forms
 * act on the most recently opened one, the -On forms on the socket given. */
void DummyNetSeedRecv(const uint8_t *buf, size_t len);
void DummyNetSeedRecvOn(void *ctx, const uint8_t *buf, size_t len);

/* Copy up to `len` captured PlatformNetSend bytes into `buf`; returns the
 * total number of bytes sent. */
size_t DummyNetGetSent(uint8_t *buf, size_t len);
size_t DummyNetGetSentOn(void *ctx, uint8_t *buf, size_t len);

/* The socket PlatformNetOpen (or an accept) handed out last — the handle the
 * -On helpers take, letting a test address one connection of several. */
void *DummyNetLastSock(void);
