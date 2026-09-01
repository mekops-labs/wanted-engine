/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX A/B firmware update. On RP2350 the backing is the BootROM's own
 * partition table; every other NuttX target has no A/B backing and reports a
 * single confirmed slot. See docs/platform-guide for the flash map. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#endif

#include <platform.h>

#ifdef CONFIG_ARCH_CHIP_RP23XX

#include <debug_trace.h>

/* BootROM function table lookup, replicated rather than included: the chip's
 * rom header lives under arch/arm/src and is off the app include path. */
#define BOOTROM_TABLE_LOOKUP_OFFSET 0x16
#define ROM_TABLE_CODE(c1, c2) ((c1) | ((c2) << 8))
#define ROM_FUNC_GET_PARTITION_TABLE_INFO ROM_TABLE_CODE('G', 'P')
#define ROM_FUNC_FLASH_RUNTIME_TO_STORAGE_ADDR ROM_TABLE_CODE('F', 'A')
#define RT_FLAG_FUNC_ARM_SEC 0x0004
#define RT_FLAG_FUNC_ARM_NONSEC 0x0010

/* Selects each partition's location and flags words, in partition order. */
#define PT_INFO_PARTITION_LOCATION_AND_FLAGS 0x0010

/* A partition's bounds are sector indices packed into the location word. */
#define PT_LOCATION_FIRST_SECTOR_MASK 0x00001fffU
#define PT_LOCATION_LAST_SECTOR_LSB 13
#define PT_LOCATION_LAST_SECTOR_MASK 0x00001fffU
#define PT_SECTOR_SIZE 4096U

/* Runtime base of XIP flash; the ROM translates it to the booted slot. */
#define XIP_RUNTIME_BASE 0x10000000U

/* Each partition contributes a location word then a flags word, after one
 * echoed flags word. */
#define PT_WORDS_PER_PARTITION 2
#define PT_QUERY_WORDS 16

typedef void *(*rom_table_lookup_fn)(uint32_t code, uint32_t mask);
typedef int (*rom_get_partition_table_info_fn)(uint32_t *out, uint32_t words,
                                               uint32_t partitionAndFlags);
typedef intptr_t (*rom_flash_runtime_to_storage_addr_fn)(uintptr_t addr);

static struct {
    bool inited;
    char activeSlot;
    int slotCount;
} g_ota;

/* The engine runs secure on this board; fall back to the non-secure table so a
 * lookup never silently yields NULL if that changes. */
static void *romFuncLookup(uint32_t code) {
    rom_table_lookup_fn lookup = (rom_table_lookup_fn)(uintptr_t)*(
        uint16_t *)(uintptr_t)BOOTROM_TABLE_LOOKUP_OFFSET;
    void *fn = lookup(code, RT_FLAG_FUNC_ARM_SEC);

    if (fn == NULL)
        fn = lookup(code, RT_FLAG_FUNC_ARM_NONSEC);
    return fn;
}

/* Storage offset the running image was loaded from, or negative on error. */
static intptr_t runningStorageOffset(void) {
    rom_flash_runtime_to_storage_addr_fn toStorage =
        (rom_flash_runtime_to_storage_addr_fn)romFuncLookup(
            ROM_FUNC_FLASH_RUNTIME_TO_STORAGE_ADDR);

    if (toStorage == NULL)
        return -ENOSYS;
    return toStorage((uintptr_t)XIP_RUNTIME_BASE);
}

/* Resolve which partition holds `offset`, as a slot letter. '\0' if none does,
 * which is how an image booted with no partition table reports. */
static char slotForOffset(intptr_t offset, int *countOut) {
    uint32_t words[PT_QUERY_WORDS];
    rom_get_partition_table_info_fn info =
        (rom_get_partition_table_info_fn)romFuncLookup(
            ROM_FUNC_GET_PARTITION_TABLE_INFO);

    if (info == NULL || offset < 0)
        return '\0';

    int filled =
        info(words, PT_QUERY_WORDS, PT_INFO_PARTITION_LOCATION_AND_FLAGS);
    if (filled < 2 || words[0] != PT_INFO_PARTITION_LOCATION_AND_FLAGS)
        return '\0';

    int count = (filled - 1) / PT_WORDS_PER_PARTITION;
    if (countOut != NULL)
        *countOut = count;

    /* The ROM answers a flash address; partition bounds are bare offsets. */
    uint32_t bare = (uint32_t)offset;
    if (bare >= XIP_RUNTIME_BASE)
        bare -= XIP_RUNTIME_BASE;

    for (int i = 0; i < count; i++) {
        uint32_t loc = words[1 + (i * PT_WORDS_PER_PARTITION)];
        uint32_t first = loc & PT_LOCATION_FIRST_SECTOR_MASK;
        uint32_t last =
            (loc >> PT_LOCATION_LAST_SECTOR_LSB) & PT_LOCATION_LAST_SECTOR_MASK;
        uint32_t start = first * PT_SECTOR_SIZE;
        uint32_t end = (last + 1) * PT_SECTOR_SIZE;

        if (bare >= start && bare < end)
            return (char)('a' + i);
    }
    return '\0';
}

int PlatformOtaInit(void) {
    intptr_t offset = runningStorageOffset();

    g_ota.slotCount = 0;
    g_ota.activeSlot = slotForOffset(offset, &g_ota.slotCount);
    g_ota.inited = true;

    if (g_ota.activeSlot == '\0') {
        DEBUG_TRACE("ota: no partition slot holds storage offset %ld",
                    (long)offset);
        return 0;
    }
    DEBUG_TRACE("ota: booted slot %c of %d, storage offset 0x%lx",
                g_ota.activeSlot, g_ota.slotCount, (long)offset);
    return 0;
}

int PlatformOtaGetBootState(platform_ota_state_t *out) {
    if (out == NULL)
        return -EINVAL;
    if (!g_ota.inited)
        return -EPERM;

    /* An image with no partition table behind it still runs; report it as the
     * first slot so the wire text stays well-defined. */
    out->active_slot = g_ota.activeSlot != '\0' ? g_ota.activeSlot : 'a';
    out->confirmed = true;
    out->pending_swap = false;
    out->last_failed_slot = '\0';
    out->boot_attempts = 0;
    out->pending_digest[0] = '\0';
    return 0;
}

#else /* !CONFIG_ARCH_CHIP_RP23XX */

int PlatformOtaInit(void) { return 0; }

int PlatformOtaGetBootState(platform_ota_state_t *out) {
    if (out == NULL)
        return -EINVAL;

    out->active_slot = 'a';
    out->confirmed = true;
    out->pending_swap = false;
    out->last_failed_slot = '\0';
    out->boot_attempts = 0;
    out->pending_digest[0] = '\0';
    return 0;
}

#endif /* CONFIG_ARCH_CHIP_RP23XX */

int PlatformOtaConfirm(void) { return 0; }

int PlatformOtaBeginWrite(void) { return -ENOSYS; }

int PlatformOtaWrite(const uint8_t *buf, size_t len) {
    (void)buf;
    (void)len;
    return -ENOSYS;
}

int PlatformOtaCommit(void) { return -ENOSYS; }

int PlatformOtaAbort(void) { return -ENOSYS; }

int PlatformOtaRollback(void) { return -ENOSYS; }
