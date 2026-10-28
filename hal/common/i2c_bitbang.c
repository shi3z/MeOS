#include <m5os/hal_i2c.h>
#include <m5os/hal_gpio.h>

#include <m5os/board_config.h>
#if defined(M5OS_BOARD_TAB5)
#  define PIN_I2C_SCL      32      /* Tab5 system bus: IO expanders, touch, RTC, sensors (M5Tab5-UserDemo BSP) */
#  define PIN_I2C_SDA      31
#else
#  define PIN_I2C_SCL      11      /* CoreS3 internal I2C (M5Unified pin table / M5GFX) */
#  define PIN_I2C_SDA      12
#endif
#define I2C_HALF_PERIOD_US 4       /* ~100 kHz */
#define I2C_STRETCH_LIMIT  1000    /* polls of SCL while a slave stretches the clock */

static int s_scl = PIN_I2C_SCL, s_sda = PIN_I2C_SDA;      /* the bus in use; hal_i2c_set_pins() switches it */

void hal_i2c_set_pins(int scl, int sda) { s_scl = scl; s_sda = sda; }

static void scl_hi_pin(int scl)
{
    hal_gpio_od_release(scl);
    for (int i = 0; i < I2C_STRETCH_LIMIT && !hal_gpio_read(scl); i++) {
        hal_delay_us(1);                      /* clock stretching */
    }
}
static void scl_lo_pin(int scl) { hal_gpio_od_drive_low(scl); }
static void sda_hi_pin(int sda) { hal_gpio_od_release(sda); }
static void sda_lo_pin(int sda) { hal_gpio_od_drive_low(sda); }
static void tick(void)          { hal_delay_us(I2C_HALF_PERIOD_US); }

void hal_i2c_init(void)
{
    hal_gpio_config_open_drain(s_scl);
    hal_gpio_config_open_drain(s_sda);
    sda_hi_pin(s_sda); scl_hi_pin(s_scl); tick();
}

static void start_pin(int scl, int sda)           /* SDA falls while SCL is high */
{
    sda_hi_pin(sda); scl_hi_pin(scl); tick();
    sda_lo_pin(sda); tick();
    scl_lo_pin(scl); tick();
}

static void stop_pin(int scl, int sda)            /* SDA rises while SCL is high */
{
    sda_lo_pin(sda); tick();
    scl_hi_pin(scl); tick();
    sda_hi_pin(sda); tick();
}

static bool write_byte_pin(int scl, int sda, uint8_t b) /* returns true on ACK */
{
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) sda_hi_pin(sda); else sda_lo_pin(sda);
        tick(); scl_hi_pin(scl); tick(); scl_lo_pin(scl);
    }
    sda_hi_pin(sda); tick();             /* release SDA: slave drives ACK */
    scl_hi_pin(scl); tick();
    bool ack = !hal_gpio_read(sda);
    scl_lo_pin(scl); tick();
    return ack;
}

static uint8_t read_byte_pin(int scl, int sda, bool ack)
{
    uint8_t v = 0;
    sda_hi_pin(sda);
    for (int i = 0; i < 8; i++) {
        tick(); scl_hi_pin(scl); tick();
        v = (uint8_t)((v << 1) | (hal_gpio_read(sda) ? 1 : 0));
        scl_lo_pin(scl);
    }
    if (ack) sda_lo_pin(sda); else sda_hi_pin(sda);
    tick(); scl_hi_pin(scl); tick(); scl_lo_pin(scl); tick();
    sda_hi_pin(sda);
    return v;
}

bool hal_i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t val)
{
    int scl = s_scl, sda = s_sda;
    start_pin(scl, sda);
    bool ok = write_byte_pin(scl, sda, (uint8_t)(addr7 << 1)) && write_byte_pin(scl, sda, reg) && write_byte_pin(scl, sda, val);
    stop_pin(scl, sda);
    return ok;
}

bool hal_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *val)
{
    int scl = s_scl, sda = s_sda;
    start_pin(scl, sda);
    bool ok = write_byte_pin(scl, sda, (uint8_t)(addr7 << 1)) && write_byte_pin(scl, sda, reg);
    if (ok) {
        start_pin(scl, sda);                           /* repeated start */
        ok = write_byte_pin(scl, sda, (uint8_t)((addr7 << 1) | 1));
        if (ok) *val = read_byte_pin(scl, sda, false); /* NACK the last byte */
    }
    stop_pin(scl, sda);
    return ok;
}

bool hal_i2c_bit_on(uint8_t a, uint8_t reg, uint8_t mask)
{
    uint8_t v;
    return hal_i2c_read_reg(a, reg, &v) && hal_i2c_write_reg(a, reg, v | mask);
}

bool hal_i2c_bit_off(uint8_t a, uint8_t reg, uint8_t mask)
{
    uint8_t v;
    return hal_i2c_read_reg(a, reg, &v) && hal_i2c_write_reg(a, reg, (uint8_t)(v & ~mask));
}

bool hal_i2c_read_regs(uint8_t addr7, uint8_t reg, uint8_t *buf, size_t len)
{
    int scl = s_scl, sda = s_sda;
    start_pin(scl, sda);
    bool ok = write_byte_pin(scl, sda, (uint8_t)(addr7 << 1)) && write_byte_pin(scl, sda, reg);
    if (ok) {
        start_pin(scl, sda);
        ok = write_byte_pin(scl, sda, (uint8_t)((addr7 << 1) | 1));
        for (size_t i = 0; ok && i < len; i++) {
            buf[i] = read_byte_pin(scl, sda, i + 1 < len); /* ACK all but the last byte */
        }
    }
    stop_pin(scl, sda);
    return ok;
}

/* 16-bit register devices (ST7123 touch, SC202CS camera) always sit on the system I2C bus
 * (PIN_I2C_SCL / PIN_I2C_SDA), even after keyboard_start() switches s_scl/s_sda to Ext.Port1. */
bool hal_i2c_read_reg16(uint8_t addr7, uint16_t reg, uint8_t *buf, size_t len)
{
    const int scl = PIN_I2C_SCL, sda = PIN_I2C_SDA;
    start_pin(scl, sda);
    bool ok = write_byte_pin(scl, sda, (uint8_t)(addr7 << 1)) &&
              write_byte_pin(scl, sda, (uint8_t)(reg >> 8)) &&
              write_byte_pin(scl, sda, (uint8_t)reg);
    if (ok) {
        start_pin(scl, sda);
        ok = write_byte_pin(scl, sda, (uint8_t)((addr7 << 1) | 1));
        for (size_t i = 0; ok && i < len; i++) buf[i] = read_byte_pin(scl, sda, i + 1 < len);
    }
    stop_pin(scl, sda);
    return ok;
}

bool hal_i2c_write_reg16(uint8_t addr7, uint16_t reg, uint8_t val)
{
    const int scl = PIN_I2C_SCL, sda = PIN_I2C_SDA;
    start_pin(scl, sda);
    bool ok = write_byte_pin(scl, sda, (uint8_t)(addr7 << 1)) &&
              write_byte_pin(scl, sda, (uint8_t)(reg >> 8)) &&
              write_byte_pin(scl, sda, (uint8_t)reg) &&
              write_byte_pin(scl, sda, val);
    stop_pin(scl, sda);
    return ok;
}
