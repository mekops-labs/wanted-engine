#include "../include/wanted.h"

#include <romfs.h>

void *RomfsMalloc(size_t s)
{
    return WantedMalloc(s);
}

void RomfsFree(void* ptr)
{
    WantedFree(ptr);
}
