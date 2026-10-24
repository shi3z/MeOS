#pragma once
#include <m5os/fs.h>
/* Flash boots the kernel. SD littlefs supplies immutable base + COW M5FS root. */
int sdroot_mount(void);
int sdroot_format(uint32_t expected_sectors); /* explicit destructive request */
int sdroot_save_probe(const char *text);
void sdroot_status(void);
bool sdroot_active(void);
int sdroot_assets_mount(void); /* /assets bulk files beside the M5FS backing store */
