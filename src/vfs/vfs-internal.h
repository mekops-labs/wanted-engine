#pragma once

#include <stdint.h>

typedef struct {
    const char *name;
    uint8_t parent;
    uint8_t type;
    int driver;
} file_t;

int VfsFindFile(int fd, const char *path, file_t *files);
