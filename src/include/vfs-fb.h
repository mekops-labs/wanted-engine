/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FB_FORMAT_RGB565 0
#define FB_FORMAT_RGB888 1

#define FB_NAME_MAX 15

/* Panel operations a backing supplies; both optional. A NULL Flush or Blank is
 * a backing whose writes already reach the panel. */
typedef struct fb_backing_ops_t {
    /* Return once the panel holds the rectangle. `wakeFd` ends the wait. */
    int (*Flush)(void *backing, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 int wakeFd);
    int (*Blank)(void *backing, bool on);
} fb_backing_ops_t;

typedef struct fb_screen_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most FB_NAME_MAX; not "observe" */
    uint16_t width;
    uint16_t height;
    uint8_t format; /* FB_FORMAT_* */
    const fb_backing_ops_t *ops;
    void *backing;
} fb_screen_desc_t;

/* Add a screen and allocate its pixels. Declaring an identical screen again
 * returns 0. Errors: -EEXIST other properties, -EINVAL bad name or geometry,
 * -ENOSPC table full, -ENOMEM no room for pixels. */
int FbScreenRegister(const fb_screen_desc_t *desc);

/* The screen's pixels and, when `stride` is not NULL, its row length in bytes.
 * NULL for an unknown name. Valid until FbScreensReset. A backing reads them
 * in its Flush. */
const uint8_t *FbScreenPixels(const char *name, uint32_t *stride);

/* Drop every screen. Only valid while no fb driver instance exists. */
void FbScreensReset(void);
