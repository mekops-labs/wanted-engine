/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <wanted-autoconf.h>

/* CONFIG_WANTED_DEBUG_TRACES: the generated header is the one place a
 * configured value comes from, so every host compiling these sources gets the
 * switch with no glue of its own. */
#if defined(CONFIG_WANTED_DEBUG_TRACES) && CONFIG_WANTED_DEBUG_TRACES
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

/* Emit one debug line with a raw write() rather than fprintf(stdout): on some
 * targets the stdout FILE stream is not bound to the console fd, while a raw
 * write to fd 1 always reaches it. Formatting once keeps the line intact. */
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
