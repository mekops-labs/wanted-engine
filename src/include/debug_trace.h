/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#if defined(DEBUG) && DEBUG
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

/* Emit one debug line with a raw write() rather than fprintf(stdout). On some
 * targets the C stdout FILE stream is not bound to the console fd — e.g. the
 * NuttX sim init task, where stdout/stderr FILE writes go nowhere but a raw
 * write() to fd 1 always reaches the console. Formatting into a stack buffer
 * and writing once keeps the line intact and works on Linux, the NuttX sim,
 * and hardware over UART. */
static inline void DebugTraceEmit(const char *file, int line, const char *func,
                                  const char *fmt, ...) {
    char buf[256];
    int off = snprintf(buf, sizeof(buf), "[%s:%d] %s: ", file, line, func);
    if (off < 0)
        return;
    if (off > (int)sizeof(buf) - 1)
        off = (int)sizeof(buf) - 1;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);
    if (n > 0)
        off += n;
    if (off > (int)sizeof(buf) - 1)
        off = (int)sizeof(buf) - 1;

    buf[off++] = '\n';
    (void)write(STDOUT_FILENO, buf, (size_t)off);
}

#define DEBUG_TRACE(fmt, ...)                                                  \
    DebugTraceEmit(__FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
#define DEBUG_TRACE(fmt, ...)
#endif
