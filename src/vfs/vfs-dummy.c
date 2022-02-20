#include <vfs.h>
#include <string.h>

static int  _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags);
static int  _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int  _Close(vfs_driver_ctx_t d, int fd);

vfs_driver_t vfs_dummy_drv = {
    .id = { 'D', 'u', 'm', 'm' },
    .filetype   = VFS_FILETYPE_REGULAR_FILE,
    .OpenAt     = _OpenAt,
    .Close      = _Close,
    .Read       = _Read,
};


char data[] = "This is dummy driver\n";
char *ptr = data;


static int  _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags)
{
    return 0;
}

static int  _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    char *b = buf;
    int r = 0;

    while(*ptr) {
        *(b++) = *(ptr++);
        r++;
    }

    return r;
}

static int  _Close(vfs_driver_ctx_t d, int fd)
{
    ptr = data;
    return 0;
}
