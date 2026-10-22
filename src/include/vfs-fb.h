/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FB_FORMAT_RGB565 0
#define FB_FORMAT_RGB888 1
#define FB_FORMAT_MONO1 2 /* 1 bit, the leftmost pixel in the high bit */
#define FB_FORMAT_GRAY4 3 /* 4 bits, the even pixel in the high nibble */

#define FB_NAME_MAX 15

/* Refresh modes of a bistable panel, as a mask in fb_screen_desc_t. */
#define FB_REFRESH_FULL 0x1    /* whole screen, clearing flash */
#define FB_REFRESH_PARTIAL 0x2 /* the changed area only */
#define FB_REFRESH_FAST 0x4    /* fastest waveform, no clearing flash */

/* Panel operations a backing supplies; all are optional. A NULL Flush or
 * Blank is a backing whose writes already reach the panel. */
typedef struct fb_backing_ops_t {
    /* Return once the panel holds the rectangle. `wakeFd` ends the wait. */
    int (*Flush)(void *backing, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 int wakeFd);
    int (*Blank)(void *backing, bool on);
    /* Grow a flush rectangle to the unit the panel writes, such as a page of
     * rows. The engine passes the grown rectangle to Flush and reports it to
     * observers, after clamping it to the screen. */
    void (*Widen)(void *backing, uint16_t *x, uint16_t *y, uint16_t *w,
                  uint16_t *h);
    /* Return once the panel holds the rectangle, refreshed in `mode` (one
     * FB_REFRESH_* bit). Called in place of Flush for a screen with refresh
     * modes. `wakeFd` ends the wait. */
    int (*Refresh)(void *backing, uint8_t mode, uint16_t x, uint16_t y,
                   uint16_t w, uint16_t h, int wakeFd);
} fb_backing_ops_t;

typedef struct fb_screen_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most FB_NAME_MAX; not "observe" */
    uint16_t width;
    uint16_t height;
    uint8_t format; /* FB_FORMAT_* */
    const fb_backing_ops_t *ops;
    void *backing;
    /* FB_REFRESH_* modes the backing offers, 0 for a screen with none. A
     * non-zero mask needs ops->Refresh. */
    uint8_t refreshModes;
} fb_screen_desc_t;

/* Add a screen and allocate its pixels. Declaring an identical screen again
 * returns 0. Errors: -EEXIST other properties, -EINVAL bad name, geometry or
 * refresh modes,
 * -ENOSPC table full, -ENOMEM no room for pixels. */
int FbScreenRegister(const fb_screen_desc_t *desc);

/* The screen's pixels and, when `stride` is not NULL, its row length in bytes.
 * NULL for an unknown name. Valid until FbScreensReset. A backing reads them
 * in its Flush. */
const uint8_t *FbScreenPixels(const char *name, uint32_t *stride);

/* Drop every screen. Only valid while no fb driver instance exists. */
void FbScreensReset(void);
