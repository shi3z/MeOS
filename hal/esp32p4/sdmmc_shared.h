#pragma once
#include <m5os/types.h>

/* Caller holds sdmmc_bus_lock().  The controller clock/reset/FIFO/IDMAC state
 * is global; slot clocks and widths are programmed independently. */
bool tab5_sdmmc_host_init_locked(void);
bool tab5_sdmmc_wait_idle_locked(uint32_t timeout_us);
bool tab5_sdmmc_set_clock_locked(unsigned slot, uint32_t frequency_khz);
bool tab5_sdmmc_set_cclk_always_on_locked(unsigned slot, bool always_on);

enum {
    TAB5_SDMMC_RSP_PRESENT = 1u << 0,
    TAB5_SDMMC_RSP_CRC     = 1u << 1,
    TAB5_SDMMC_RSP_LONG    = 1u << 2,
};

bool tab5_sdmmc_command_locked(unsigned slot, unsigned command, uint32_t argument,
                               unsigned response_flags, bool send_init,
                               uint32_t response[4]);

