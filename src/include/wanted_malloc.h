/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdlib.h>

void *WantedMalloc(size_t s);
void WantedFree(void *ptr);
size_t WantedGetAllocatedMem(void);
