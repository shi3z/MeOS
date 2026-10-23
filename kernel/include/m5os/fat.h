#pragma once
#include <m5os/vfs.h>

/* Single FAT12/16/32 volume at /sd, 512-byte sectors, VFAT UTF-8 long names and short aliases.
 * Formatting, writes, exFAT and automatic remount are intentionally unavailable. */
int fat_mount_readonly(blockdev_t *bd);
int fat_unmount(void);              /* refuses while files/directories are in use */
bool fat_is_mounted(void);
