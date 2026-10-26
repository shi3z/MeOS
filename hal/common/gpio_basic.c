/* GPIO helpers built on hal/gpio_ll.h (inline register access) and two ROM
 * functions.  No IDF driver/gpio.c (it takes FreeRTOS spinlocks). */
#include <m5os/hal_gpio.h>
#include <m5os/board_config.h>
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#if M5OS_BOARD_TAB5
#include "esp_cpu.h"
#endif

void hal_gpio_config_output(int pin, int level)
{
    esp_rom_gpio_pad_select_gpio(pin);                                  /* IO_MUX -> GPIO function */
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);/* GPIO matrix -> plain output */
    gpio_ll_set_level(&GPIO, pin, level);
    gpio_ll_output_enable(&GPIO, pin);
}

void hal_gpio_config_input_pullup(int pin)
{
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_ll_output_disable(&GPIO, pin);
    gpio_ll_input_enable(&GPIO, pin);
    gpio_ll_pulldown_dis(&GPIO, pin);
    gpio_ll_pullup_en(&GPIO, pin);
}

void hal_gpio_config_open_drain(int pin)
{
    esp_rom_gpio_pad_select_gpio(pin);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_set_level(&GPIO, pin, 0);          /* level stays 0; only the output enable toggles */
    gpio_ll_input_enable(&GPIO, pin);
    gpio_ll_pullup_en(&GPIO, pin);
    gpio_ll_output_disable(&GPIO, pin);
}

void hal_gpio_od_release(int pin)   { gpio_ll_output_disable(&GPIO, pin); }
void hal_gpio_od_drive_low(int pin) { gpio_ll_output_enable(&GPIO, pin); }
int  hal_gpio_read(int pin)         { return gpio_ll_get_level(&GPIO, pin); }
void hal_gpio_write(int pin, int l) { gpio_ll_set_level(&GPIO, pin, l); }
void hal_delay_us(uint32_t us)      { esp_rom_delay_us(us); }

void hal_i2c_fast_tick(void)
{
#if M5OS_BOARD_TAB5
    uint32_t cycles=esp_rom_get_cpu_ticks_per_us();
    uint32_t began=esp_cpu_get_cycle_count();
    while((uint32_t)(esp_cpu_get_cycle_count()-began)<cycles) __asm__ volatile("nop");
#else
    esp_rom_delay_us(1); /* the cycle-counter fast path is only used by Tab5 touch */
#endif
}
