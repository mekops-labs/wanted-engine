#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t vfs_filetype_t;

#define VFS_FILETYPE_UNKNOWN 0
#define VFS_FILETYPE_BLOCK_DEVICE 1
#define VFS_FILETYPE_CHARACTER_DEVICE 2
#define VFS_FILETYPE_DIRECTORY 3
#define VFS_FILETYPE_REGULAR_FILE 4
#define VFS_FILETYPE_SOCKET_DGRAM 5
#define VFS_FILETYPE_SOCKET_STREAM 6
#define VFS_FILETYPE_SYMBOLIC_LINK 7

typedef int vfs_oflags_t;
// open flags
#define VFS_O_CREAT 0100
#define VFS_O_DIRECTORY 0200000 /* Must be a directory.  */
#define VFS_O_EXCL 0200
#define VFS_O_TRUNC 01000
#define VFS_O_APPEND 02000
#define VFS_O_NONBLOCK 04000
#define VFS_O_DSYNC 010000 /* Synchronize data.  */
#define VFS_O_SYNC 04010000
#define VFS_O_RSYNC VFS_O_SYNC /* Synchronize read operations.  */
#define VFS_O_RDWR 02
#define VFS_O_WRONLY 01
#define VFS_O_RDONLY 00

// seek whence
typedef uint8_t vfs_whence_t;

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

typedef uint8_t vfs_riflags_t;
#define VFS_RIFLAGS_RECV_PEEK 1
#define VFS_RIFLAGS_RECV_WAITALL 2

typedef uint8_t vfs_roflags_t;
#define VFS_ROFLAGS_RECV_DATA_TRUNCATED 1

typedef uint8_t vfs_siflags_t;

typedef uint8_t vfs_sdflags_t;
#define VFS_SDFLAGS_RD 1
#define VFS_SDFLAGS_WR 2

typedef struct vfs_stat_t {
    uint32_t dev;            // Device/driver id containing the file.
    uint32_t ino;            // File serial number.
    vfs_filetype_t filetype; // File type.
    uint32_t
        size; // File size of regular files, in bytes. For symbolic links, the
              // length in bytes of the pathname contained in the symbolic link.
    uint32_t nlink; // Number of hardlinks pointing to this file.
    uint64_t atim;  // Last access time
    uint64_t mtim;  // Last modification time
    uint64_t ctim;  // Last file status change
    int oflags;     // Oflags
} vfs_stat_t;

typedef struct vfs_dirent_t {
    uint64_t d_next;   // The offset of the next directory entry stored in this
                       // directory.
    uint64_t d_ino;    // The serial number of the file referred to by this
                       // directory entry.
    uint32_t d_namlen; // The length of the name of the directory entry.
    uint32_t
        d_type; // The type of the file referred to by this directory entry.
} vfs_dirent_t;

typedef struct vfs_driver_ctx_t *vfs_driver_ctx_t;

typedef struct vfs_driver_t {
    /* driver ID magic number */
    union {
        const char id[4];
        uint32_t bytesId;
    };

    vfs_driver_ctx_t ctx;    // Driver's internal data
    vfs_filetype_t filetype; // Driver's type

    /* Register sub-driver */
    int (*Register)(vfs_driver_ctx_t d, const char *path,
                    const struct vfs_driver_t *driver);

    /* Destroy this driver */
    int (*Destroy)(struct vfs_driver_t *d);

    /* FS operations */

    int (*Open)(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
    int (*OpenAt)(vfs_driver_ctx_t d, int fd, const char *path,
                  vfs_oflags_t flags);
    int (*Close)(vfs_driver_ctx_t d, int fd);
    int (*Stat)(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
    int (*StatSet)(vfs_driver_ctx_t d, int fd, vfs_stat_t stat);
    int (*Unlink)(vfs_driver_ctx_t d, int fd, const char *path);

    int (*Read)(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
    int (*Write)(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
    int (*Seek)(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                long *pos);

    int (*ReadDir)(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                   uint64_t *cookie, size_t *bufUsed);

    /* Network operations */

    int (*SockAccept)(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags,
                      int *newFd);
    int (*SockRecv)(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte,
                    vfs_riflags_t iflags, vfs_roflags_t *oflags);
    int (*SockSend)(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte,
                    vfs_sdflags_t flags);
    int (*SockShutdown)(vfs_driver_ctx_t d, int fd, vfs_sdflags_t flags);
} vfs_driver_t;

#define VFS_STDIN 0
#define VFS_STDOUT 1
#define VFS_STDERR 2

typedef struct vfs_ctx_t *vfs_ctx_t;

vfs_ctx_t VfsInit();
void VfsDestroy(vfs_ctx_t *c);
void VfsSetPrivileged(vfs_ctx_t c, bool privileged);
int VfsRegister(vfs_ctx_t c, const char *path, const vfs_driver_t *driver);

int VfsOpen(vfs_ctx_t c, const char *path, vfs_oflags_t flags);
int VfsOpenAt(vfs_ctx_t c, int fd, const char *path, vfs_oflags_t flags);
int VfsClose(vfs_ctx_t c, int fd);
int VfsStatAt(vfs_ctx_t c, int fd, const char *path, vfs_stat_t *stat);
int VfsStat(vfs_ctx_t c, int fd, vfs_stat_t *stat);
int VfsStatSet(vfs_ctx_t c, int fd, vfs_stat_t stat);
int VfsRead(vfs_ctx_t c, int fd, void *buf, size_t nbyte);
int VfsWrite(vfs_ctx_t c, int fd, const void *buf, size_t nbyte);
int VfsSeek(vfs_ctx_t c, int fd, long off, vfs_whence_t whence, long *pos);
int VfsReadDir(vfs_ctx_t c, int fd, void *buf, size_t bufLen, uint64_t *cookie,
               size_t *bufUsed);
int VfsUnlink(vfs_ctx_t c, int fd, const char *path);

int VfsSockAccept(vfs_ctx_t c, int fd, vfs_oflags_t flags, int *newFd);
int VfsSockRecv(vfs_ctx_t c, int fd, void *buf, size_t nbyte,
                vfs_riflags_t iflags, vfs_roflags_t *oflags);
int VfsSockSend(vfs_ctx_t c, int fd, const void *buf, size_t nbyte,
                vfs_sdflags_t flags);
int VfsSockShutdown(vfs_ctx_t c, int fd, vfs_sdflags_t flags);
