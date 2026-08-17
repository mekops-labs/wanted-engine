/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Process-wide per-wapp log buffers. A wapp whose console is the "log" driver
 * has stdout/stderr captured here, read back by the supervisor. Each wapp gets
 * a fixed ring, so only its most recent output is kept. */

#include <stdbool.h>
#include <stddef.h>

#include <wanted-api.h> /* WAPP_MAX_NAME_LEN */

/* Reserved log name for the engine's own error channel. The leading dot is
 * outside the image-reference grammar (validInstallRef requires [A-Za-z0-9_]
 * first), so no installable image can claim the slot. */
#define WANTED_ENGINE_LOG_NAME ".engine"

/* The previous boot's engine log, served beside WANTED_ENGINE_LOG_NAME and
 * reserved the same way. Present only when the store found one. */
#define WANTED_PREV_LOG_NAME ".engine.prev"

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

/* True if a log slot exists for `name` (the wapp has captured output). */
bool LogStoreHas(log_store_t *s, const char *name);

/* Copy the names of up to `max` in-use slots into `names` (each
 * NUL-terminated). Returns the count written. Lets a read surface enumerate the
 * wapps with a live log. */
size_t LogStoreList(log_store_t *s, char names[][WAPP_MAX_NAME_LEN],
                    size_t max);

/* Append the engine's own error channel to WANTED_ENGINE_LOG_NAME's ring, so a
 * board with no console can be asked what happened. Declared for wanted_log.h,
 * which is a header of static inlines and must not pull this one in. */
/* NOLINTNEXTLINE(readability-redundant-declaration) */
void WantedLogCapture(const void *buf, size_t n);

/* Adopt memory a reset does not clear: its log becomes the previous
 * boot's, and this boot writes to the other half. Idempotent; call once
 * before the supervisor starts; absent such memory, keeps no previous log. */
void LogStorePersistInit(log_store_t *s);

/* Forget the adopted memory so the next LogStorePersistInit adopts it again.
 * The store is a process-wide singleton, so this is how a test performs the
 * process restart a reset would otherwise do. */
void LogStorePersistDetach(log_store_t *s);
