#pragma once
#include <m5os/blockdev.h>

typedef struct {
    bool probed, ready, high_capacity, write_protected;
    uint32_t sectors;
    uint32_t last_status;
    uint8_t last_r1;
    uint8_t bus_width;
    uint16_t clock_khz;
    const char *transport;
    const char *detail;
} sdcard_status_t;

/* Tab5 only. Manual probe; no auto format/mount. Transfers are synchronous.
 * No card-detect pin: a no-response result cannot distinguish absence from a fault. */
blockdev_t *sdcard_probe(void);
void sdcard_status(sdcard_status_t *out);
/* Legacy name retained for the host-tested SPI transport. This powers LDO4 and
 * leaves the dedicated slot pins in a safe state; native SDMMC then remuxes them. */
bool sdcard_spi_power_init(void);
