/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

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
 * of entries stored, or -ENOSPC if the table is full. This is the test write
 * path: the real PlatformRegistryWrite streams an image to a host file under an
 * install ref, which the dummy platform cannot do. */
int DummyRegistrySeed(const reg_entry_t *entries, size_t count);

/* ── Wapp runtime-state mock control (dummy-wapps.c) ────────────────────── */

/* Clear the in-memory wapp runtime-state table. Call in TEST_SETUP. */
void DummyWappStateReset(void);

/* Populate the wapp runtime-state table from `states`, upserting by name.
 * Returns the number stored, or -ENOSPC if the table is full. Lets a test put
 * a wapp into a known status (e.g. RUNNING) without a real WASM runtime, so
 * PlatformWappGetState — and the control-plane reads layered on it — return
 * deterministic data. */
int DummyWappStateSeed(const wapp_state_t *states, size_t count);

/* ── Network mock control (dummy-net.c) ─────────────────────────────────── */

/* Reset socket pool, buffers, and controllable results. Call in TEST_SETUP. */
void DummyNetReset(void);

/* Make the next PlatformNetOpen return NULL (simulate open failure). */
void DummyNetSetOpenFail(int fail);

/* Set the value PlatformNetConnect returns (0 = success, <0 = failure). */
void DummyNetSetConnectResult(int result);

/* Set the value PlatformNetAccept returns. */
void DummyNetSetAcceptResult(int result);

/* Seed bytes that PlatformNetRecv will return (drained across calls). */
void DummyNetSeedRecv(const uint8_t *buf, size_t len);

/* Copy up to `len` captured PlatformNetSend bytes into `buf`; returns the
 * total number of bytes sent. */
size_t DummyNetGetSent(uint8_t *buf, size_t len);
