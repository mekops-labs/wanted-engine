#pragma once

// ROMFS_FILEHDR_NEXT file mode/type values

#define ROMFS_TYPE_HARDLINK    0    ///> FILEHDR_INFO: Link destination file header
#define ROMFS_TYPE_DIRECTORY   1    ///> FILEHDR_INFO: First file's header
#define ROMFS_TYPE_FILE        2    ///> FILEHDR_INFO: Unused, must be zero
#define ROMFS_TYPE_SOFTLINK    3    ///> FILEHDR_INFO: Unused, must be zero
#define ROMFS_TYPE_BLOCKDEV    4    ///> FILEHDR_INFO: 16/16 bits major/minor number
#define ROMFS_TYPE_CHARDEV     5    ///> FILEHDR_INFO: 16/16 bits major/minor number
#define ROMFS_TYPE_SOCKET      6    ///> FILEHDR_INFO: Unused, must be zero
#define ROMFS_TYPE_FIFO        7    ///> FILEHDR_INFO: Unused, must be zero

#define ROMFS_MODE_EXEC        8    ///> Modifier for TYPE_DIRECTORY and TYPE_FILE


// compatibility defines
#define O_READ          0x0
#define O_WRONLY        0x1
#define O_RDWR          0x2
#define O_CREAT         0x4
#define O_DIRECTORY     0x8
#define O_EXCL          0x10
#define O_TRUNC         0x11
#define O_APPEND        0x12
#define O_DSYNC         0x14
#define O_NONBLOCK      0x18
#define O_RSYNC         0x20
#define O_SYNC          0x21

int RomfsLoad(uint8_t * img, size_t imgSize);

int RomfsFdStat(int fd);
int RomfsOpenAt(int fd, const char *path, int flags, int mode);
