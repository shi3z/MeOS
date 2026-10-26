#ifndef MEOS_COW_BLOCKDEV_H
#define MEOS_COW_BLOCKDEV_H
#include "cow.h"
#include <m5os/blockdev.h>
/* Caller owns transaction fences and serializes the whole M5FS operation group.
 * A failed operation requires discarding mounted M5FS state before recovery.
 * This adapter never begins or commits implicitly. */
void cow_blockdev_init(blockdev_t *, struct cow *);
#endif
