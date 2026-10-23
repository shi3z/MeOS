#pragma once
#include <m5os/blockdev.h>

typedef struct {
    bool probed, ready, high_capacity;
    uint32_t sectors;
    uint8_t last_r1;
    const char *detail;
} sdcard_status_t;

/* Tab5 only. Manual probe; no auto format/mount and no writes to media.
 * No card-detect pin: a no-response result cannot distinguish absence from a fault. */
blockdev_t *sdcard_probe(void);
void sdcard_status(sdcard_status_t *out);
bool sdcard_spi_power_init(void); /* HAL: dedicated slot pins and LDO4; never touches SDMMC */
