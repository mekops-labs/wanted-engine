#include <vfs-drivers.h>

// TODO: make it dynamic in the long run

vfs_driver_table_t global_driver_table[] = {
    { "null",       VfsNullInit,       },
    { "9p",         Vfs9PInit,         },
    { "config",     VfsConfigInit,     },
    { "platform",   VfsPlatformFsInit, },
    { "rom",        VfsRomfsInit,      },
    { "socket",     VfsSocketInit,     },
    { "virt",       VfsVirtualInit,    },
    { "wanted",     VfsWantedInit,     },
    { NULL, NULL },
};