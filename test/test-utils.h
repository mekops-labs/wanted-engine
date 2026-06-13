/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>

/* Returns 1 if `needle` (nlen bytes) appears anywhere inside `hay` (hlen bytes). */
int HasBytes(const void *hay, size_t hlen, const char *needle, size_t nlen);
