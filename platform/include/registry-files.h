/* SPDX-License-Identifier: Apache-2.0 */

/* Small named records, one file each, in one directory of a POSIX filesystem.
 * A put lands the record whole or not at all: it is staged in the directory
 * and renamed over the target. Names that start with '.' or '_' are skipped. */

#ifndef REGISTRY_FILES_H
#define REGISTRY_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <registry-store.h>

int RegFilesPut(const char *dir, const char *name, const uint8_t *data,
                size_t len);
/* Bytes read, -ENOENT, or -EMSGSIZE when the record is longer than `cap`. */
int RegFilesGet(const char *dir, const char *name, uint8_t *buf, size_t cap);
int RegFilesDel(const char *dir, const char *name);
/* Calls `visit` for each name until it returns non-zero. Returns the number of
 * names visited, or a negative errno. A missing directory has none. */
int RegFilesList(const char *dir, int (*visit)(const char *name, void *arg),
                 void *arg);

/* The file name of an entry record, "<name><sep><version>.rec", and back. */
int RegFilesKeyName(char *out, size_t cap, const registry_key_t *key);
bool RegFilesParseKey(const char *file, registry_key_t *key);

#endif /* REGISTRY_FILES_H */
