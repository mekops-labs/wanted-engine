#pragma once

#include <stdint.h>

/* Reset all in-memory file/directory/fd state. Call in TEST_SETUP. */
void DummyFsReset(void);

/* Reset the monotonic clock counter to zero and reseed the PRNG. */
void DummyClockReset(void);

/* Advance the monotonic clock by `ns` nanoseconds without sleeping. */
void DummyClockAdvance(uint64_t ns);
