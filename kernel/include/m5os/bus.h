#pragma once
/* The CoreS3 LCD and the SD card share SCLK/MOSI (and GPIO35 = MISO = LCD D/C).
 * Every transaction on either must hold this lock.  It degrades to a no-op
 * before the scheduler runs or when interrupts are masked (it cannot block
 * there), which is fine: nothing else runs concurrently in those situations. */
void spi_bus_lock(void);
void spi_bus_unlock(void);

/* ESP32-P4 has one SDMMC controller with two slots: Tab5 microSD is slot 0 and
 * the ESP32-C6 Wi-Fi link is slot 1. Command, FIFO and IDMAC registers are
 * shared even though clocks and bus widths are per-slot. */
void sdmmc_bus_lock(void);
void sdmmc_bus_unlock(void);
