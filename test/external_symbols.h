#pragma once

extern unsigned char test_wasi[];
extern unsigned int test_wasi_len;

/* Legacy romfs-format twin of test_wasi, kept alive only for the legacy
 * vfs_register / vfs_openclose tests. Phase 8 deletes both the romfs driver
 * and these tests. */
extern unsigned char test_wasi_romfs[];
extern unsigned int test_wasi_romfs_len;
