/*
 * PSRAM as set up by the IDF start-up code (esp_psram_init, run from start_cpu0_default - no
 * FreeRTOS involved).  The size is public (esp_psram_get_size()); the virtual address is chosen by
 * the MMU allocator and is NOT exposed, so it is found the way the hardware knows it: the start of
 * the external-RAM virtual window, checked against the MMU - the first page there must translate to
 * PSRAM physical address 0.  If that check fails we report "no PSRAM" instead of guessing.
 */
#include <m5os/hal_mem.h>
#include <m5os/board_config.h>
#include "sdkconfig.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#include "esp_private/esp_mmu_map_private.h"
#include "esp_mmu_map.h"
#endif

void hal_psram_region(uintptr_t *base, size_t *size)
{
    *base = 0;
    *size = 0;
#if CONFIG_SPIRAM
    if (!esp_psram_is_initialized()) return;
    size_t sz = esp_psram_get_size();
    void *vaddr = NULL;
    if (esp_mmu_paddr_to_vaddr(0, MMU_TARGET_PSRAM0, MMU_VADDR_DATA, &vaddr) != ESP_OK || !vaddr) return;
    *base = (uintptr_t)vaddr;
    *size = sz - M5OS_PSRAM_RESERVE;                 /* the reserve stays with the IDF heap, see hal_psram_reserve_region */
#endif
}

void hal_psram_reserve_region(uintptr_t *base, size_t *size)
{
    uintptr_t b;
    size_t s;
    hal_psram_region(&b, &s);
    *base = s ? b + s : 0;
    *size = s ? M5OS_PSRAM_RESERVE : 0;
}
