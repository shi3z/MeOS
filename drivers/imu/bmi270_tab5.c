/*
 * BMI270 accelerometer, enough to notice that the device was picked up.
 *
 * Start-up sequence per the BMI270 datasheet / Bosch's reference driver (bmi2.c): soft reset, check the chip
 * ID, switch off advanced power saving, write the 8 KB configuration blob through INIT_DATA in 32-byte pieces
 * (INIT_ADDR_0/1 tell the chip where in its program memory each piece goes, in 16-bit words), start the
 * initialisation and wait for "init OK" in INTERNAL_STATUS.  Then the accelerometer is enabled; we poll its
 * data registers.  No interrupt line, no gyroscope.
 */
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/imu.h>
#include <m5os/hal_i2c.h>
#include <m5os/hal_gpio.h>
#include <m5os/hal_time.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include "bmi270_config.h"

#define BMI_ADDR        0x68
#define REG_CHIP_ID     0x00
#define REG_ACC_DATA    0x0C            /* X, Y, Z: 16 bits each, little endian */
#define REG_INT_STATUS  0x21            /* INTERNAL_STATUS: bits 3:0 = 1 when the initialisation succeeded */
#define REG_ACC_CONF    0x40
#define REG_ACC_RANGE   0x41
#define REG_INIT_CTRL   0x59
#define REG_INIT_ADDR0  0x5B
#define REG_INIT_ADDR1  0x5C
#define REG_INIT_DATA   0x5E
#define REG_PWR_CONF    0x7C
#define REG_PWR_CTRL    0x7D
#define REG_CMD         0x7E

#define CHIP_ID             0x24
#define CMD_SOFT_RESET      0xB6
#define ACC_CONF_25HZ       0xA6        /* performance filter, normal bandwidth, 25 Hz output rate */
#define ACC_RANGE_8G        0x02
#define PWR_CTRL_ACC_ON     0x04
#define INIT_OK             0x01
#define CHUNK               32          /* bytes per burst, as in the reference driver */
#define RESET_MS            5           /* datasheet: 2 ms after a soft reset */
#define INIT_POLL_MS        10
#define INIT_TIMEOUT_MS     200

static bool s_ready;

static bool wr(uint8_t reg, uint8_t v) { return hal_i2c_sys_write_reg(BMI_ADDR, reg, v); }

bool imu_ready(void) { return s_ready; }

bool imu_init(void)
{
    s_ready = false;
    uint8_t id = 0;
    if (!hal_i2c_sys_read_regs(BMI_ADDR, REG_CHIP_ID, &id, 1) || id != CHIP_ID) return false;
    if (!wr(REG_CMD, CMD_SOFT_RESET)) return false;
    task_sleep(RESET_MS);
    if (!wr(REG_PWR_CONF, 0x00)) return false;                  /* advanced power saving off: the config file can only be loaded this way */
    hal_delay_us(500);
    if (!wr(REG_INIT_CTRL, 0x00)) return false;
    for (unsigned i = 0; i < BMI270_CONFIG_BYTES; i += CHUNK) {
        uint8_t where[2] = { (uint8_t)((i / 2) & 0x0F), (uint8_t)((i / 2) >> 4) };
        if (!wr(REG_INIT_ADDR0, where[0]) || !wr(REG_INIT_ADDR1, where[1])) return false;
        if (!hal_i2c_sys_write_regs(BMI_ADDR, REG_INIT_DATA, &bmi270_config_file[i], CHUNK)) return false;
        if ((i % 1024) == 0) task_yield();                      /* a second of I2C: let others run in between */
    }
    if (!wr(REG_INIT_CTRL, 0x01)) return false;
    uint8_t status = 0;
    for (unsigned t = 0; t < INIT_TIMEOUT_MS; t += INIT_POLL_MS) {
        task_sleep(INIT_POLL_MS);
        if (hal_i2c_sys_read_regs(BMI_ADDR, REG_INT_STATUS, &status, 1) && (status & 0x0F) == INIT_OK) break;
    }
    if ((status & 0x0F) != INIT_OK) return false;
    if (!wr(REG_ACC_CONF, ACC_CONF_25HZ) || !wr(REG_ACC_RANGE, ACC_RANGE_8G) || !wr(REG_PWR_CTRL, PWR_CTRL_ACC_ON)) return false;
    task_sleep(INIT_POLL_MS);
    s_ready = true;
    return true;
}

bool imu_read_accel(int16_t xyz[3])
{
    uint8_t b[6];
    if (!s_ready || !hal_i2c_sys_read_regs(BMI_ADDR, REG_ACC_DATA, b, sizeof b)) return false;
    for (int i = 0; i < 3; i++) xyz[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    return true;
}
#endif
