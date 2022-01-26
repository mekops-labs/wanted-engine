#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint8_t vfs_filetype_t;

#define VFS_FILETYPE_UNKNOWN             0
#define VFS_FILETYPE_BLOCK_DEVICE        1
#define VFS_FILETYPE_CHARACTER_DEVICE    2
#define VFS_FILETYPE_DIRECTORY           3
#define VFS_FILETYPE_REGULAR_FILE        4
#define VFS_FILETYPE_SOCKET_DGRAM        5
#define VFS_FILETYPE_SOCKET_STREAM       6
#define VFS_FILETYPE_SYMBOLIC_LINK       7

typedef struct vfs_filestat_t {
    uint32_t dev;               // Device/driver id containing the file.
    uint32_t ino;               // File serial number.
    vfs_filetype_t filetype;    // File type.
    uint32_t size;              // File size of regular files, in bytes. For symbolic links, the length in bytes of the pathname contained in the symbolic link.
    uint64_t atim;              // Last access time
    uint64_t mtim;              // Last modification time
    uint64_t ctim;              // Last file status change
} vfs_filestat_t;

typedef struct vfs_dirent_t {
    uint32_t d_next;            // The offset of the next directory entry stored in this directory.
    uint32_t d_ino;             // The serial number of the file referred to by this directory entry.
    uint16_t d_namlen;          // The length of the name of the directory entry.
    vfs_filetype_t d_type;      // The type of the file referred to by this directory entry.
} vfs_dirent_t;

typedef struct vfs_driver_t {
    const char id[4];
    int  (*Open)(const char *path, int flags);
    int  (*OpenAt)(int fd, const char *path, int flags);
    int  (*Close)(int fd);
    int  (*FdStat)(int fd, vfs_filestat_t *stat);
    int  (*FdStatAt)(int fd, const char *path, vfs_filestat_t *stat);
    int  (*Read)(int fd, void *buf, size_t nbyte);
    int  (*Write)(int fd, void *buf, size_t nbyte);
    long (*Seek)(int fd, long off, int whence);
    long (*Tell)(int fd);
    int  (*ReadDir)(int fd, void *buf, size_t bufLen, uint32_t *cookie, size_t *bufUsed);
} vfs_driver_t;
