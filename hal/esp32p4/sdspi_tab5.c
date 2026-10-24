/* Tab5 SD slot in SPI mode: CLK43, CMD/MOSI44, DAT0/MISO39, DAT3/CS42.
 * DAT1/2 stay inputs with pullups. Supply is LDO4 at 3.3V (vendor BSP).
 * No SDMMC clocks, DMA, interrupt status or slot registers are touched: Wi-Fi keeps slot 1.
 * LDO4 belongs exclusively to this driver; low-level register access avoids the
 * FreeRTOS critical sections inside esp_ldo_acquire_channel when called at runtime. */
#include <m5os/sdcard.h>
#include <m5os/hal_sdspi.h>
#include <m5os/hal_gpio.h>
#include <m5os/arch.h>
#include "hal/ldo_ll.h"

#define CLK 43
#define MOSI 44
#define MISO 39
#define CS 42
static bool s_powered;
static bool s_slow = true;

bool sdcard_spi_power_init(void)
{
    if (s_powered) return true;
    uint8_t dref, mul; bool rail;
    ldo_ll_voltage_to_dref_mul(3, 3300, &dref, &mul, &rail); /* channel 4 = unit 3 */
    uint32_t irq = arch_irq_disable();
    ldo_ll_adjust_voltage(3, dref, mul, rail);
    ldo_ll_set_owner(3, LDO_LL_UNIT_OWNER_SW);
    ldo_ll_enable_ripple_suppression(3, true);
    ldo_ll_enable(3, true);
    arch_irq_restore(irq);
    hal_gpio_config_output(CS, 1);
    hal_gpio_config_output(CLK, 0);
    hal_gpio_config_output(MOSI, 1);
    hal_gpio_config_input_pullup(MISO);
    hal_gpio_config_input_pullup(40);
    hal_gpio_config_input_pullup(41);
    hal_delay_us(10000);
    s_powered = true;
    return true;
}
void hal_sdspi_begin(void) { }
void hal_sdspi_end(void) { }
void hal_sdspi_cs(bool selected) { hal_gpio_write(CS, selected ? 0 : 1); }
void hal_sdspi_set_slow(bool slow) { s_slow = slow; }
uint8_t hal_sdspi_xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int bit = 7; bit >= 0; bit--) {
        hal_gpio_write(MOSI, (out >> bit) & 1);
        if (s_slow) hal_delay_us(2);
        hal_gpio_write(CLK, 1);
        if (s_slow) hal_delay_us(2);
        in = (uint8_t)((in << 1) | hal_gpio_read(MISO));
        hal_gpio_write(CLK, 0);
    }
    return in;
}
