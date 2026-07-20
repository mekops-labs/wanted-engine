/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Always-compiled error channel — config and launch faults must stay
 * diagnosable where DEBUG_TRACE compiles away. Raw write() as in DEBUG_TRACE:
 * on some targets the stderr FILE stream is not bound to the console fd. */
static inline void WantedLogErrEmit(const char *fmt, ...) {
    char buf[256];
    int off = snprintf(buf, sizeof(buf), "wanted: ");
    if (off < 0)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);
    if (n > 0)
        off += n;
    if (off > (int)sizeof(buf) - 1)
        off = (int)sizeof(buf) - 1;

    buf[off++] = '\n';
    (void)write(STDERR_FILENO, buf, (size_t)off);
}

#define LOG_ERROR(fmt, ...) WantedLogErrEmit(fmt, ##__VA_ARGS__)

/* Describe a wapp launch result. -1 is the trap/bad-image sentinel and carries
 * no errno, so strerror would invent a cause; anything else is a negated
 * errno. */
static inline const char *wappErrText(int err) {
    if (err == -1)
        return "the wapp image failed to load or trapped at startup";
    return strerror(err < 0 ? -err : err);
}
