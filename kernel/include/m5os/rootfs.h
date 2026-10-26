#pragma once
#include <m5os/types.h>
#include <m5os/blockdev.h>
bool rootfs_mount(blockdev_t *sd);
/* Current root backing, recorded after a successful root mount. */
bool rootfs_is_persistent(void);
