/* SPDX-License-Identifier: Apache-2.0 */

#include "test-utils.h"

#include <string.h>

int HasBytes(const void *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp((const char *)hay + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

void TarHeader(uint8_t hdr[512], const char *name, uint32_t size,
               char typeflag) {
    memset(hdr, 0, 512);
    strncpy((char *)hdr, name, 99);
    for (int i = 10; i >= 0; i--) {
        hdr[124 + i] = (uint8_t)('0' + (size & 7));
        size >>= 3;
    }
    hdr[124 + 11] = '\0';
    hdr[156] = (uint8_t)typeflag;
    memcpy(hdr + 257, "ustar", 5);
}

reg_entry_t MakeEntry(const char *name, const char *version, size_t size) {
    reg_entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.name, name, WAPP_MAX_NAME_LEN - 1);
    strncpy(e.version, version, WAPP_MAX_VERSION_LEN - 1);
    e.size = size;
    return e;
}
