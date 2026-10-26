/*
 * Root filesystem selection.
 *   1. Tab5 first tries an existing littlefs/COW M5FS root on SD.
 *   2. A supplied legacy M5FS block device is tried if no SD-root was mounted.
 *   3. Otherwise a RAM disk is formatted and mounted (volatile, lost at reset).
 * Boot NEVER formats the SD card; failed or absent media remains untouched.
 */
#include <m5os/vfs.h>
#include <m5os/rootfs.h>
#include <m5os/m5ex.h>
#include <m5os/wasm.h>
#include <m5os/fs.h>
#include <m5os/errno.h>
#include <m5os/kprintf.h>
#include <m5os/kmalloc.h>
#include <m5os/board_config.h>
#if defined(M5OS_BOARD_TAB5)
#include <m5os/sdroot.h>
#endif
#include "../../drivers/ramdisk/ramdisk.h"

#define RAMDISK_BLOCKS_PSRAM_LARGE 16384u     /* 8 MiB on boards with >= 16 MiB PSRAM (Tab5) */
#define RAMDISK_BLOCKS_PSRAM       4096u      /* 2 MiB on 8 MiB PSRAM boards */
#define RAMDISK_BLOCKS_SRAM        128u       /* 64 KiB: boards without PSRAM keep it in internal RAM */

static bool s_persistent;
bool rootfs_is_persistent(void) { return s_persistent; }

static void seed(const char *path, const char *text)
{
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return;
    size_t n = 0;
    while (text[n]) n++;
    vfs_write(fd, text, n);
    vfs_close(fd);
}

bool rootfs_mount(blockdev_t *sd)
{
    bool persistent = false;
#if defined(M5OS_BOARD_TAB5)
    int sdrc = sdroot_mount();
    persistent = sdrc == ERR_OK;
    kprintf("sdroot: mount %d (no automatic format)\n", sdrc);
#endif
    if (sd && !persistent) {
        int rc = fs_mount(sd, 0);
        if (rc == ERR_OK) {
            persistent = true;
        } else {
            kprintf("rootfs: SD card has no usable M5FS (%d); NOT formatting it, using RAM disk\n", rc);
        }
    }
    if (!persistent) {
        kmem_stats_t mp;
        kmem_psram_stats(&mp);
        uint32_t blocks = mp.total >= (16u * 1024u * 1024u) ? RAMDISK_BLOCKS_PSRAM_LARGE
                        : (mp.total ? RAMDISK_BLOCKS_PSRAM : RAMDISK_BLOCKS_SRAM);
        blockdev_t *rd = ramdisk_create(blocks);
        if (!rd || fs_format(rd, 0, blocks) != ERR_OK || fs_mount(rd, 0) != ERR_OK) {
            kprintf("rootfs: cannot create RAM disk root\n");
            return false;
        }
    }
    int rc = vfs_mount("/", &vfs_m5fs, NULL);
    if (rc != ERR_OK) return false;
    s_persistent = persistent;
#if defined(M5OS_BOARD_TAB5)
    if(sdroot_active())kprintf("sdroot: assets mount %d\n",sdroot_assets_mount());
#endif
    kprintf("rootfs: M5FS on %s mounted at /\n", persistent ? "SD card (persistent)" : "RAM disk (volatile)");

    if (!persistent) {
        seed("/system/version", "M5OS 0.1\n");
        seed("/home/hello.txt", "Hello from M5FS via the VFS!\n");
        seed("/apps/README.txt", "Applications live here.\n");
        int count;
        const m5ex_image_t *img = m5ex_embedded(&count);                 /* sample .m5ex apps built into the firmware */
        for (int i = 0; i < count; i++) {
            char path[VFS_PATH_MAX];
            ksnprintf(path, sizeof path, "/apps/%s.m5ex", img[i].name);
            int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
            if (fd >= 0) { vfs_write(fd, img[i].data, img[i].size); vfs_close(fd); }
        }
    }
    wasm_store_boot();                           /* persistent /apps, or built-in samples on a volatile root */
#if defined(M5OS_BOARD_TAB5)
    if (sdroot_active()) kputs("SDROOT BOOT PASS: persistent root; RAM disk not allocated; kernel remains in Flash\n");
#endif
    return true;
}
