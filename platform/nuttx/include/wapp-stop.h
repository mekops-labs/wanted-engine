/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

/* Cooperative-stop plumbing internal to the NuttX platform. The signal handler
 * records the interrupt per worker and PlatformClockNanoSleep consumes it to
 * turn an early-woken sleep into EINTR, which NuttX reports as success. */

/* Read and clear the calling worker's pending stop interrupt. Returns true if
 * the stop handler had marked this thread since the last consume. */
bool PlatformStopInterruptConsume(void);
