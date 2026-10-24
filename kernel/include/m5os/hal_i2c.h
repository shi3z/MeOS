#pragma once
#include <m5os/types.h>

/* Bit-banged I2C master on the CoreS3 internal bus (SCL=GPIO11, SDA=GPIO12).
 * Chosen over the I2C peripheral to avoid clock/reset/GPIO-matrix setup and
 * any dependency on IDF's (FreeRTOS-locked) i2c driver; ~100 kHz is plenty
 * for power-IC setup.  Replace with the controller later if needed. */
/* Transfers are serialized (including read-modify-write). Task context only, or single-threaded boot.
 * Select/init pins before starting their users; runtime clients must not change another client's bus. */
void hal_i2c_set_pins(int scl, int sda);   /* selects legacy 8-bit helpers; call hal_i2c_init() after */
void hal_i2c_init(void);
bool hal_i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t val);
bool hal_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *val);
bool hal_i2c_bit_on(uint8_t addr7, uint8_t reg, uint8_t mask);
bool hal_i2c_bit_off(uint8_t addr7, uint8_t reg, uint8_t mask);
bool hal_i2c_read_regs(uint8_t addr7, uint8_t reg, uint8_t *buf, size_t len);  /* auto-increment read */
bool hal_i2c_read_reg16(uint8_t addr7, uint16_t reg, uint8_t *buf, size_t len);  /* devices with 16-bit register numbers (ST7123 touch, SC202CS camera) */
bool hal_i2c_write_reg16(uint8_t addr7, uint16_t reg, uint8_t val);
/* Fixed system bus, 8-bit register ADDRESS and arbitrary data length (INA226, IO expanders).
 * Does not change the legacy bus selected for the keyboard accessory. */
bool hal_i2c_sys_read_regs(uint8_t addr7, uint8_t reg, uint8_t *buf, size_t len);

bool hal_i2c_sys_write_reg(uint8_t addr7, uint8_t reg, uint8_t val);
/* Atomic masked read-modify-write on the fixed system bus. */
bool hal_i2c_sys_update_reg(uint8_t addr7, uint8_t reg, uint8_t mask, uint8_t value);

typedef enum { HAL_I2C_OK, HAL_I2C_NACK, HAL_I2C_TIMEOUT, HAL_I2C_INVALID } hal_i2c_result_t;
typedef struct { uint32_t ok, nack, timeout, invalid; } hal_i2c_stats_t;
/* Per-call results; unlike a global last-error, these remain valid after unlock. */
hal_i2c_result_t hal_i2c_sys_read_regs_ex(uint8_t addr,uint8_t reg,uint8_t *buf,size_t len);
hal_i2c_result_t hal_i2c_sys_write_reg_ex(uint8_t addr,uint8_t reg,uint8_t value);
void hal_i2c_get_stats(hal_i2c_stats_t *out);

/* System bus only; caller selects a fast-capable device. */
bool hal_i2c_read_reg16_fast(uint8_t addr7,uint16_t reg,uint8_t *buf,size_t len);
