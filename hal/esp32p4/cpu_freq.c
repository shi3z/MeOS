/*
 * CPU clock switching for suspend.
 *
 * The CPU runs from the 360 MHz CPLL through a divider (360, 180 or 90 MHz on this chip revision: IDF's
 * rtc_clk_cpu_freq_mhz_to_config() lists what is possible).  Changing only the divider is the safe way to
 * slow down: the PLL stays on, and the buses that matter keep their rate - with 360 MHz the memory bus is
 * 180 MHz and the APB 90 MHz, with 90 MHz both are 90 MHz, so the peripherals on the APB do not notice.
 * IDF's switch also updates the ROM's "CPU ticks per microsecond", which esp_rom_delay_us() - and so the
 * bit-banged I2C - uses; that is why we go through it instead of poking the divider registers.
 *
 * Switching to the 40 MHz crystal would turn the PLL off, and the PSRAM, the display and the Wi-Fi bridge
 * may depend on it, so it is refused here.
 */
#include <m5os/hal_cpu.h>
#include <m5os/arch.h>
#include "soc/rtc.h"
#include "esp_rom_sys.h"

unsigned hal_cpu_mhz(void) { return esp_rom_get_cpu_ticks_per_us(); }

bool hal_cpu_set_mhz(unsigned mhz)
{
    rtc_cpu_freq_config_t cfg;
    if (!rtc_clk_cpu_freq_mhz_to_config(mhz, &cfg)) return false;
    if (cfg.source != SOC_CPU_CLK_SRC_CPLL) return false;
    uint32_t irq = arch_irq_disable();                  /* nothing may run on a half-switched clock */
    rtc_clk_cpu_freq_set_config(&cfg);
    arch_irq_restore(irq);
    return true;
}
