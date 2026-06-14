/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <wanted-api.h>

/* Returns 1 if `needle` (nlen bytes) appears anywhere inside `hay` (hlen bytes). */
int HasBytes(const void *hay, size_t hlen, const char *needle, size_t nlen);

/* Build a 512-byte ustar header for `name`/`size`/`typeflag` (test fixture). */
void TarHeader(uint8_t hdr[512], const char *name, uint32_t size, char typeflag);

/* Build a registry entry from name/version/size (test fixture). */
reg_entry_t MakeEntry(const char *name, const char *version, size_t size);
