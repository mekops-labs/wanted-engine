#include "test-utils.h"

#include <string.h>

int HasBytes(const void *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp((const char *)hay + i, needle, nlen) == 0) return 1;
    }
    return 0;
}
