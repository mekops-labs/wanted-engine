#include <vfs.h>

int VfsRegister(vfs_driver_t *driver, const char *path);

int VfsOpen(const char *path, int flags);
int VfsOpenAt(int fd, const char *path, int flags);
int VfsClose(int fd);
int VfsFdStat(int fd, vfs_fdstat_t *stat);
int VfsFileStatAt(int fd, const char *path, vfs_filestat_t *stat);
int VfsRead(int fd, void *buf, size_t nbyte);
int VfsWrite(int fd, const void *buf, size_t nbyte);
int VfsSeek(int fd, long off, int whence, long *pos);
int VfsTell(int fd, long *pos);
int VfsReadDir(int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed);
