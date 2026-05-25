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
 * path: the real PlatformRegistryWrite derives name/version from a parsed WASM
 * manifest, which the dummy platform cannot do. */
int DummyRegistrySeed(const reg_entry_t *entries, size_t count);
