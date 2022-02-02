#pragma once

#if DEBUG
#   include <stdio.h>
#   define DEBUG_TRACE(fmt, ...) fprintf(stderr, "[%s:%d]: " fmt "\n",__FUNCTION__ ,__LINE__ , ##__VA_ARGS__)
#else
#   define DEBUG_TRACE(fmt, ...)
#endif
