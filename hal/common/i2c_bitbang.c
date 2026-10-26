#include <m5os/hal_i2c.h>
#include <m5os/hal_gpio.h>
#include <m5os/mutex.h>

#include <m5os/board_config.h>
#if defined(M5OS_BOARD_CARDPUTER)
#  define PIN_I2C_SCL      9       /* Cardputer-ADV only: SYS_SCL/SYS_SDA (M5Unified pin table). On the original */
#  define PIN_I2C_SDA      8       /* Cardputer these two pins are the keyboard decoder's selects - see keyboard.c */
#elif defined(M5OS_BOARD_TAB5)
#  define PIN_I2C_SCL      32      /* Tab5 system bus: IO expanders, touch, RTC, sensors (M5Tab5-UserDemo BSP) */
#  define PIN_I2C_SDA      31
#else
#  define PIN_I2C_SCL      11      /* CoreS3 internal I2C (M5Unified pin table / M5GFX) */
#  define PIN_I2C_SDA      12
#endif
#define I2C_HALF_PERIOD_US 4       /* ~100 kHz */
#define I2C_STRETCH_LIMIT  1000    /* polls of SCL while a slave stretches the clock */

static int s_scl = PIN_I2C_SCL, s_sda = PIN_I2C_SDA;      /* the bus in use; hal_i2c_set_pins() switches it */

/* One lock covers both physical buses, including read-modify-write operations.
 * Board bring-up precedes the scheduler: no lock is needed until a task exists.
 * Task context only; callers must not invoke I2C from an ISR. */
static mutex_t s_lock = MUTEX_INIT;
static bool bus_lock(void)
{
    if (!task_current()) return false;
    mutex_lock(&s_lock);
    return true;
}
static void bus_unlock(bool locked) { if (locked) mutex_unlock(&s_lock); }

void hal_i2c_set_pins(int scl, int sda)
{
    bool locked = bus_lock();
    s_scl = scl; s_sda = sda;
    bus_unlock(locked);
}

static hal_i2c_result_t s_result;
static hal_i2c_stats_t s_stats;
static bool scl_hi_pin(int scl)
{
    hal_gpio_od_release(scl);
    for(unsigned i=0;i<I2C_STRETCH_LIMIT;i++) {
        if(hal_gpio_read(scl)) return true;
        hal_delay_us(1);
    }
    s_result=HAL_I2C_TIMEOUT;
    return false;
}
static void scl_lo_pin(int scl) { hal_gpio_od_drive_low(scl); }
static void sda_hi_pin(int sda) { hal_gpio_od_release(sda); }
static void sda_lo_pin(int sda) { hal_gpio_od_drive_low(sda); }
/* Protected by s_lock, including restoration before leaving a fast transfer. */
static unsigned s_half_period_us = I2C_HALF_PERIOD_US;
static void tick(void)
{
    if(s_half_period_us==1) hal_i2c_fast_tick();
    else hal_delay_us(s_half_period_us);
}

void hal_i2c_init(void)
{
    bool locked = bus_lock();
    s_result=HAL_I2C_OK;
    hal_gpio_config_open_drain(s_scl);
    hal_gpio_config_open_drain(s_sda);
    sda_hi_pin(s_sda); scl_hi_pin(s_scl); tick();
    /* A CPU reset can interrupt a slave in the middle of a report. Release
     * a stuck SDA with nine clocks and STOP before the first address phase. */
    if (!hal_gpio_read(s_sda)) {
        for (unsigned i=0;i<9;i++) {
            scl_lo_pin(s_scl); tick();
            scl_hi_pin(s_scl); tick();
        }
        sda_lo_pin(s_sda); tick();
        scl_hi_pin(s_scl); tick();
        sda_hi_pin(s_sda); tick();
    }
    bus_unlock(locked);
}

static void start_pin(int scl, int sda)           /* SDA falls while SCL is high */
{
    sda_hi_pin(sda); if(!scl_hi_pin(scl)) return; tick();
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
    if(s_result!=HAL_I2C_OK) return false;
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) sda_hi_pin(sda); else sda_lo_pin(sda);
        tick(); if(!scl_hi_pin(scl)) return false; tick(); scl_lo_pin(scl);
    }
    sda_hi_pin(sda); tick();             /* release SDA: slave drives ACK */
    if(!scl_hi_pin(scl)) return false;
    tick();
    bool ack = !hal_gpio_read(sda);
    scl_lo_pin(scl); tick();
    if(!ack) s_result=HAL_I2C_NACK;
    return ack;
}

static uint8_t read_byte_pin(int scl, int sda, bool ack)
{
    uint8_t v = 0;
    sda_hi_pin(sda);
    for (int i = 0; i < 8; i++) {
        tick(); if(!scl_hi_pin(scl)) return 0; tick();
        v = (uint8_t)((v << 1) | (hal_gpio_read(sda) ? 1 : 0));
        scl_lo_pin(scl);
    }
    if (ack) sda_lo_pin(sda); else sda_hi_pin(sda);
    tick(); if(!scl_hi_pin(scl)) return 0; tick(); scl_lo_pin(scl); tick();
    sda_hi_pin(sda);
    return v;
}

/* Caller holds s_lock. reg_bytes describes the register ADDRESS, not its data width.
 * INA226 uses an 8-bit register address with 16-bit data; GT911/ST712x use 16-bit addresses. */
static bool transfer(int scl, int sda, uint8_t addr, uint16_t reg, unsigned reg_bytes,
                     const uint8_t *tx, uint8_t *rx, size_t len)
{
    s_result=HAL_I2C_OK;
    if(addr>0x7f || !len || (!tx && !rx)) { s_result=HAL_I2C_INVALID; s_stats.invalid++; return false; }
    start_pin(scl, sda);
    bool ok = write_byte_pin(scl, sda, (uint8_t)(addr << 1));
    if (ok && reg_bytes == 2) ok = write_byte_pin(scl, sda, (uint8_t)(reg >> 8));
    if (ok) ok = write_byte_pin(scl, sda, (uint8_t)reg);
    if (ok && rx) {
        start_pin(scl, sda);
        ok = write_byte_pin(scl, sda, (uint8_t)((addr << 1) | 1));
        for (size_t i = 0; ok && i < len; i++) {
            rx[i] = read_byte_pin(scl, sda, i + 1 < len); ok=s_result==HAL_I2C_OK;
        }
    } else {
        for (size_t i = 0; ok && i < len; i++) ok = write_byte_pin(scl, sda, tx[i]);
    }
    stop_pin(scl, sda);
    if(s_result==HAL_I2C_OK) s_stats.ok++;
    else if(s_result==HAL_I2C_TIMEOUT) s_stats.timeout++;
    else if(s_result==HAL_I2C_NACK) s_stats.nack++;
    return ok && s_result==HAL_I2C_OK;
}

bool hal_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    bool locked = bus_lock();
    bool ok = transfer(s_scl, s_sda, addr, reg, 1, &val, NULL, 1);
    bus_unlock(locked);
    return ok;
}

bool hal_i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    bool locked = bus_lock();
    bool ok = transfer(s_scl, s_sda, addr, reg, 1, NULL, buf, len);
    bus_unlock(locked);
    return ok;
}
bool hal_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *val)
{
    return hal_i2c_read_regs(addr, reg, val, 1);
}

static bool modify(uint8_t addr, uint8_t reg, uint8_t mask, bool set)
{
    bool locked = bus_lock();
    uint8_t v;
    bool ok = transfer(s_scl, s_sda, addr, reg, 1, NULL, &v, 1);
    if (ok) {
        v = set ? (uint8_t)(v | mask) : (uint8_t)(v & ~mask);
        ok = transfer(s_scl, s_sda, addr, reg, 1, &v, NULL, 1);
    }
    bus_unlock(locked);
    return ok;
}
bool hal_i2c_bit_on(uint8_t a, uint8_t reg, uint8_t mask) { return modify(a, reg, mask, true); }
bool hal_i2c_bit_off(uint8_t a, uint8_t reg, uint8_t mask) { return modify(a, reg, mask, false); }

/* System-bus helpers never change s_scl/s_sda: touch, camera and power monitoring
 * can coexist with the keyboard accessory on Ext.Port1. */
bool hal_i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t *buf, size_t len)
{
    bool locked = bus_lock();
    bool ok = transfer(PIN_I2C_SCL, PIN_I2C_SDA, addr, reg, 2, NULL, buf, len);
    bus_unlock(locked);
    return ok;
}
bool hal_i2c_write_reg16(uint8_t addr, uint16_t reg, uint8_t val)
{
    bool locked = bus_lock();
    bool ok = transfer(PIN_I2C_SCL, PIN_I2C_SDA, addr, reg, 2, &val, NULL, 1);
    bus_unlock(locked);
    return ok;
}
bool hal_i2c_sys_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    bool locked = bus_lock();
    bool ok = transfer(PIN_I2C_SCL, PIN_I2C_SDA, addr, reg, 1, NULL, buf, len);
    bus_unlock(locked);
    return ok;
}

bool hal_i2c_sys_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    bool locked = bus_lock();
    bool ok = transfer(PIN_I2C_SCL, PIN_I2C_SDA, addr, reg, 1, &val, NULL, 1);
    bus_unlock(locked); return ok;
}
bool hal_i2c_sys_update_reg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value)
{
    bool locked = bus_lock(); uint8_t v;
    bool ok = transfer(PIN_I2C_SCL, PIN_I2C_SDA, addr, reg, 1, NULL, &v, 1);
    if (ok) { v = (v & ~mask) | (value & mask);
        ok = transfer(PIN_I2C_SCL, PIN_I2C_SDA, addr, reg, 1, &v, NULL, 1); }
    bus_unlock(locked); return ok;
}

hal_i2c_result_t hal_i2c_sys_read_regs_ex(uint8_t addr,uint8_t reg,uint8_t *buf,size_t len)
{
    bool locked=bus_lock();
    transfer(PIN_I2C_SCL,PIN_I2C_SDA,addr,reg,1,NULL,buf,len);
    hal_i2c_result_t result=s_result; bus_unlock(locked); return result;
}
hal_i2c_result_t hal_i2c_sys_write_reg_ex(uint8_t addr,uint8_t reg,uint8_t value)
{
    bool locked=bus_lock();
    transfer(PIN_I2C_SCL,PIN_I2C_SDA,addr,reg,1,&value,NULL,1);
    hal_i2c_result_t result=s_result; bus_unlock(locked); return result;
}
void hal_i2c_get_stats(hal_i2c_stats_t *out)
{
    if(!out) return;
    bool locked=bus_lock(); *out=s_stats; bus_unlock(locked);
}

bool hal_i2c_read_reg16_fast(uint8_t addr,uint16_t reg,uint8_t *buf,size_t len)
{
    bool locked=bus_lock();
    s_half_period_us=1;
    bool ok=transfer(PIN_I2C_SCL,PIN_I2C_SDA,addr,reg,2,NULL,buf,len);
    s_half_period_us=I2C_HALF_PERIOD_US;
    bus_unlock(locked);
    return ok;
}
