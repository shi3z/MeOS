#include <m5os/bus.h>
#include <m5os/mutex.h>
#include <m5os/arch.h>

static mutex_t s_spi_bus = MUTEX_INIT;
static mutex_t s_sdmmc_bus = MUTEX_INIT;

/* Remember whether we really took it: the answer can differ between lock and
 * unlock calls only if the caller's context changes, which callers must not do. */
static bool can_block(void) { return sched_started() && arch_irq_enabled(); }

void spi_bus_lock(void)   { if (can_block()) mutex_lock(&s_spi_bus); }
void spi_bus_unlock(void) { if (can_block()) mutex_unlock(&s_spi_bus); }
void sdmmc_bus_lock(void)   { if (can_block()) mutex_lock(&s_sdmmc_bus); }
void sdmmc_bus_unlock(void) { if (can_block()) mutex_unlock(&s_sdmmc_bus); }
