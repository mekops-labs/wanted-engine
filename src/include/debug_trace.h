#pragma once

#if DEBUG
#   include <stdio.h>
#   define DEBUG_TRACE(fmt, ...) fprintf(stdout, "[%s:%d] %s: " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#else
#   define DEBUG_TRACE(fmt, ...)
#endif
