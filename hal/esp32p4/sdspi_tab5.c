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
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_rom_gpio.h"

#define CLK 43
#define MOSI 44
#define MISO 39
#define CS 42
static bool s_powered;
static bool s_slow = true;

static void card_pin_high_impedance(int pin)
{
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_ll_output_disable(&GPIO, pin);
    gpio_ll_input_enable(&GPIO, pin);
    gpio_ll_pullup_dis(&GPIO, pin);
    gpio_ll_pulldown_dis(&GPIO, pin);
}

bool sdcard_spi_power_init(void)
{
    if (s_powered) return true;
    /* A CPU reset does not necessarily remove LDO4 power.  The previous
     * firmware may therefore have left the card latched in SPI mode.  Merely
     * disabling LDO4 is insufficient while high GPIOs or internal pull-ups
     * can parasitically power the card through its I/O protection diodes.
     * Float every signal during the off interval, then establish D3 high
     * immediately before restoring the SD-only rail. */
    card_pin_high_impedance(CLK);
    card_pin_high_impedance(MOSI);
    card_pin_high_impedance(MISO);
    card_pin_high_impedance(40);
    card_pin_high_impedance(41);
    card_pin_high_impedance(CS);
    uint32_t irq = arch_irq_disable();
    /* Boot ROM or a previous firmware may have left VO4 under PMU hardware
     * ownership.  Take software ownership before clearing XPD; otherwise the
     * write can leave the card powered across a host reset. */
    ldo_ll_set_owner(3, LDO_LL_UNIT_OWNER_SW);
    ldo_ll_enable(3, false);
    arch_irq_restore(irq);
    hal_delay_us(250000);

    hal_gpio_config_output(CS, 1);
    hal_gpio_config_output(CLK, 0);
    hal_gpio_config_output(MOSI, 1);
    hal_gpio_config_input_pullup(MISO);
    hal_gpio_config_input_pullup(40);
    hal_gpio_config_input_pullup(41);

    uint8_t dref, mul; bool rail;
    ldo_ll_voltage_to_dref_mul(3, 3300, &dref, &mul, &rail); /* channel 4 = unit 3 */
    irq = arch_irq_disable();
    ldo_ll_adjust_voltage(3, dref, mul, rail);
    ldo_ll_set_owner(3, LDO_LL_UNIT_OWNER_SW);
    ldo_ll_enable_ripple_suppression(3, true);
    ldo_ll_enable(3, true);
    arch_irq_restore(irq);
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
