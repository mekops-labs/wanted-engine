#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "romfs-internal.h"

static inline
uint32_t ReadBE32(const uint8_t *buf, size_t offset)
{
    return ((((uint32_t)*(buf + offset)     & 0xff) << 24) |
            (((uint32_t)*(buf + offset + 1) & 0xff) << 16) |
            (((uint32_t)*(buf + offset + 2) & 0xff) << 8) |
             ((uint32_t)*(buf + offset + 3) & 0xff));
}

int RomfsVolumeConfigure(const uint8_t *buf, volume_t *vol)
{
    if (memcmp(buf, VOLHDR_MAGIC_STR, 8) != 0) {
        return -EINVAL;
    }

    vol->size = ReadBE32(buf, VOLHDR_SIZE_OFF);
    vol->chksum = ReadBE32(buf, VOLHDR_CHKSUM_OFF);
    vol->name = (const char *)&buf[VOLHDR_VOLNAME_OFF];
    vol->rootOff = ROMFS_ALIGNUP(VOLHDR_VOLNAME_OFF + strlen(vol->name) + 1);

    return 0;
}

int RomfsGetNodeHdr(const romfs_t *rm, uint32_t offset, nodehdr_t *nd)
{
    uint8_t *buf = rm->img;

    if (offset > rm->vol.size || offset > rm->size) {
        return -EINVAL;
    }

    buf = buf + offset;

    nd->next = ReadBE32(buf, FILEHDR_NEXT_OFF) & ~(FILEHDR_NEXT_MODE_MASK);
    nd->mode = buf[FILEHDR_NEXT_OFF + 3] & 0xF;
    nd->info = ReadBE32(buf, FILEHDR_INFO_OFF);
    nd->size = ReadBE32(buf, FILEHDR_SIZE_OFF);
    nd->chksum = ReadBE32(buf, FILEHDR_CHKSUM_OFF);
    nd->name = (const char *)&buf[FILEHDR_NAME_OFF];
    nd->dataOff = offset + ROMFS_ALIGNUP(FILEHDR_NAME_OFF + strlen(nd->name) + 1);

    return 0;
}
