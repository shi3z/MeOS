#pragma once
#include <m5os/types.h>

/* Minimal GPIO boundary.  Pin numbers are chip GPIO numbers. */
void hal_gpio_config_output(int pin, int initial_level);
void hal_gpio_config_input_pullup(int pin);     /* plain input, internal pull-up (keyboard rows, SD MISO) */
/* Open-drain emulation for I2C: "release" = input (external pull-up pulls high),
 * "drive_low" = output 0.  Internal pull-up is enabled as a fallback. */
void hal_gpio_config_open_drain(int pin);
void hal_gpio_od_release(int pin);
void hal_gpio_od_drive_low(int pin);
int  hal_gpio_read(int pin);
void hal_gpio_write(int pin, int level);
void hal_delay_us(uint32_t us);
void hal_i2c_fast_tick(void); /* >=1us cycle-counted delay; fast I2C only */
