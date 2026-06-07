#pragma once

/* Process-wide per-wapp log buffers.
 *
 * A wapp whose console is the "log" driver has its stdout/stderr captured here
 * instead of sharing the platform console; the supervisor reads it back via
 * /dev/wanted/wapps/<name>/log. Each wapp gets a fixed-size ring buffer, so a
 * chatty or long-lived wapp keeps only its most recent output. */

#include <stddef.h>

typedef struct log_store_t log_store_t;

/* Process-global singleton, created on first use (NULL on allocation failure).
 * Shared by the log console driver (append) and the control plane (read). */
log_store_t *LogStore(void);

/* Append n bytes of wapp `name`'s output. Silently drops the oldest bytes once
 * the per-wapp ring is full. No-op on NULL store / name. */
void LogStoreAppend(log_store_t *s, const char *name, const void *buf,
                    size_t n);

/* Copy up to cap bytes of `name`'s buffered output (oldest→newest) into out.
 * Returns the number of bytes copied (0 if absent / empty). */
size_t LogStoreRead(log_store_t *s, const char *name, char *out, size_t cap);
