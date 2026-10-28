/*
 * Root filesystem selection.
 *   1. An SD card that already holds an M5FS volume is mounted as "/" (persistent).
 *   2. Otherwise a RAM disk is formatted and mounted (volatile, lost at reset).
 * A card without M5FS is NEVER formatted automatically: that would destroy
 * whatever the user keeps on it.
 */
#include <m5os/vfs.h>
#include <m5os/m5ex.h>
#include <m5os/wasm.h>
#include <m5os/fs.h>
#include <m5os/errno.h>
#include <m5os/kprintf.h>
#include <m5os/kmalloc.h>
#include "../../drivers/ramdisk/ramdisk.h"

#define RAMDISK_BLOCKS_PSRAM_LARGE 16384u     /* 8 MiB on boards with >= 16 MiB PSRAM (Tab5) */
#define RAMDISK_BLOCKS_PSRAM       4096u      /* 2 MiB on 8 MiB PSRAM boards */
#define RAMDISK_BLOCKS_SRAM        128u       /* 64 KiB: boards without PSRAM keep it in internal RAM */

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
    if (sd) {
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
    wasm_store_boot();                           /* installed WebAssembly apps come back from flash (Tab5 only) */
    return true;
}
