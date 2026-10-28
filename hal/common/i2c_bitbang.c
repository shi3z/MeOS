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

static void scl_hi(void)
{
    hal_gpio_od_release(s_scl);
    for (int i = 0; i < I2C_STRETCH_LIMIT && !hal_gpio_read(s_scl); i++) {
        hal_delay_us(1);                      /* clock stretching */
    }
}
static void scl_lo(void) { hal_gpio_od_drive_low(s_scl); }
static void sda_hi(void) { hal_gpio_od_release(s_sda); }
static void sda_lo(void) { hal_gpio_od_drive_low(s_sda); }
static void tick(void)   { hal_delay_us(I2C_HALF_PERIOD_US); }

void hal_i2c_init(void)
{
    hal_gpio_config_open_drain(s_scl);
    hal_gpio_config_open_drain(s_sda);
    sda_hi(); scl_hi(); tick();
}

static void start(void)           /* SDA falls while SCL is high */
{
    sda_hi(); scl_hi(); tick();
    sda_lo(); tick();
    scl_lo(); tick();
}

static void stop(void)            /* SDA rises while SCL is high */
{
    sda_lo(); tick();
    scl_hi(); tick();
    sda_hi(); tick();
}

static bool write_byte(uint8_t b) /* returns true on ACK */
{
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) sda_hi(); else sda_lo();
        tick(); scl_hi(); tick(); scl_lo();
    }
    sda_hi(); tick();             /* release SDA: slave drives ACK */
    scl_hi(); tick();
    bool ack = !hal_gpio_read(s_sda);
    scl_lo(); tick();
    return ack;
}

static uint8_t read_byte(bool ack)
{
    uint8_t v = 0;
    sda_hi();
    for (int i = 0; i < 8; i++) {
        tick(); scl_hi(); tick();
        v = (uint8_t)((v << 1) | (hal_gpio_read(s_sda) ? 1 : 0));
        scl_lo();
    }
    if (ack) sda_lo(); else sda_hi();
    tick(); scl_hi(); tick(); scl_lo(); tick();
    sda_hi();
    return v;
}

bool hal_i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t val)
{
    start();
    bool ok = write_byte((uint8_t)(addr7 << 1)) && write_byte(reg) && write_byte(val);
    stop();
    return ok;
}

bool hal_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *val)
{
    start();
    bool ok = write_byte((uint8_t)(addr7 << 1)) && write_byte(reg);
    if (ok) {
        start();                                       /* repeated start */
        ok = write_byte((uint8_t)((addr7 << 1) | 1));
        if (ok) *val = read_byte(false);               /* NACK the last byte */
    }
    stop();
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
    start();
    bool ok = write_byte((uint8_t)(addr7 << 1)) && write_byte(reg);
    if (ok) {
        start();
        ok = write_byte((uint8_t)((addr7 << 1) | 1));
        for (size_t i = 0; ok && i < len; i++) {
            buf[i] = read_byte(i + 1 < len);          /* ACK all but the last byte */
        }
    }
    stop();
    return ok;
}

bool hal_i2c_read_reg16(uint8_t addr7, uint16_t reg, uint8_t *buf, size_t len)
{
    start();
    bool ok = write_byte((uint8_t)(addr7 << 1)) && write_byte((uint8_t)(reg >> 8)) && write_byte((uint8_t)reg);
    if (ok) {
        start();
        ok = write_byte((uint8_t)((addr7 << 1) | 1));
        for (size_t i = 0; ok && i < len; i++) buf[i] = read_byte(i + 1 < len);
    }
    stop();
    return ok;
}
