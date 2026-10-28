#pragma once
#include <m5os/types.h>

/* Bit-banged I2C master on the CoreS3 internal bus (SCL=GPIO11, SDA=GPIO12).
 * Chosen over the I2C peripheral to avoid clock/reset/GPIO-matrix setup and
 * any dependency on IDF's (FreeRTOS-locked) i2c driver; ~100 kHz is plenty
 * for power-IC setup.  Replace with the controller later if needed. */
void hal_i2c_set_pins(int scl, int sda);   /* use another pair of pins from now on (one bus at a time); call hal_i2c_init() after */
void hal_i2c_init(void);
bool hal_i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t val);
bool hal_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *val);
bool hal_i2c_bit_on(uint8_t addr7, uint8_t reg, uint8_t mask);
bool hal_i2c_bit_off(uint8_t addr7, uint8_t reg, uint8_t mask);
bool hal_i2c_read_regs(uint8_t addr7, uint8_t reg, uint8_t *buf, size_t len);  /* auto-increment read */
bool hal_i2c_read_reg16(uint8_t addr7, uint16_t reg, uint8_t *buf, size_t len);  /* devices with 16-bit register numbers (ST7123 touch, SC202CS camera) */
bool hal_i2c_write_reg16(uint8_t addr7, uint16_t reg, uint8_t val);
