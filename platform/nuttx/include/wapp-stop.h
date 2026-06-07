#pragma once

#include <stdbool.h>

/* Cooperative-stop plumbing internal to the NuttX platform.
 *
 * PlatformWappStop signals a stopped wapp's worker so a host call it is blocked
 * in returns and the interpreter can honour the terminate flag. The signal
 * handler records the interrupt per worker; PlatformClockNanoSleep consumes it
 * to turn an early-woken sleep into EINTR (NuttX reports the woken sleep as
 * success, so the timer return cannot signal the interrupt on its own). */

/* Read and clear the calling worker's pending stop interrupt. Returns true if
 * the stop handler had marked this thread since the last consume. */
bool PlatformStopInterruptConsume(void);
