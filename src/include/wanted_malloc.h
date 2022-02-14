#pragma once

#ifndef WANTED_CUSTOM_MALLOC
#   include <stdlib.h>
#   define WantedMalloc(x)  malloc(x)
#   define WantedFree(x)  free(x)
#else
void *WantedMalloc(size_t s);
void WantedFree(void* ptr);
#endif
