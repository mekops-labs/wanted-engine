#pragma once

#include <stdlib.h>

void *WantedMalloc(size_t s);
void WantedFree(void* ptr);
size_t WantedGetAllocatedMem();
