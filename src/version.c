/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>

#include <version.h>

#define VERSION_PARTS 3

typedef struct {
    unsigned long part[VERSION_PARTS];
    bool past_tag; /* a `-<commit>` suffix: built after the tag, so newer */
    bool parsed;
} version_t;

/* Read `<major>.<minor>.<patch>`, dropping a leading `v`. A missing part is
 * zero; anything from the first `-` or `+` is the suffix. */
static version_t parse(const char *s) {
    version_t v = {{0, 0, 0}, false, false};

    if (s == NULL || *s == '\0')
        return v;
    if (*s == 'v' || *s == 'V')
        s++;

    for (size_t i = 0; i < VERSION_PARTS; i++) {
        if (*s < '0' || *s > '9')
            break;
        while (*s >= '0' && *s <= '9') {
            v.part[i] = v.part[i] * 10 + (unsigned long)(*s - '0');
            s++;
        }
        v.parsed = true;
        if (*s != '.')
            break;
        s++;
    }

    v.past_tag = (*s == '-' || *s == '+');
    return v;
}

bool VersionNewer(const char *a, const char *b) {
    version_t va = parse(a);
    version_t vb = parse(b);

    /* A version nothing can be read from never displaces one that parses. */
    if (!va.parsed)
        return false;
    if (!vb.parsed)
        return true;

    for (size_t i = 0; i < VERSION_PARTS; i++) {
        if (va.part[i] != vb.part[i])
            return va.part[i] > vb.part[i];
    }

    /* Same release: a build past the tag is newer than the tag itself. Two
     * builds past one tag carry no ordering, so neither displaces the other. */
    return va.past_tag && !vb.past_tag;
}
