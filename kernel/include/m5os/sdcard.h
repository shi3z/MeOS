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

/* Per-transfer wall times, including preemption. Classes are <=1/8/32/128
 * sectors. Reset touches diagnostics only, including while the root is mounted. */
#define SDCARD_IO_CLASSES 4u
typedef struct {
    uint32_t calls, blocks, errors;
    uint64_t wait_us, prepare_us, dma_us, finish_us, hold_us;
    uint32_t max_wait_us, max_prepare_us, max_dma_us, max_finish_us, max_hold_us;
    uint32_t max_polls;
} sdcard_io_class_t;
typedef struct { sdcard_io_class_t read[SDCARD_IO_CLASSES], write[SDCARD_IO_CLASSES]; } sdcard_io_stats_t;
/* False on transports without these diagnostics; no probe or media IO. */
bool sdcard_get_io_stats(sdcard_io_stats_t *out, bool reset);
/* Legacy name retained for the host-tested SPI transport. This powers LDO4 and
 * leaves the dedicated slot pins in a safe state; native SDMMC then remuxes them. */
bool sdcard_spi_power_init(void);
