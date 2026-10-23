#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <m5os/hal_i2c.h>
#include <m5os/power.h>

static uint16_t words[256];
static int fail_reg = -1;
static uint8_t direction = 0xb9;
static bool expander_ok = true;
bool hal_i2c_sys_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (addr == 0x44) {
        assert(len == 1);
        if (!expander_ok) return false;
        *buf = reg == 3 ? direction : reg == 5 ? 0x89 : 0x40;
        return true;
    }
    assert(addr == 0x41 && len == 2);
    if (reg == fail_reg) return false;
    buf[0] = words[reg] >> 8; buf[1] = words[reg];
    return true;
}
int main(void)
{
    power_sample_t p;
    words[0xfe] = 0x5449; words[0xff] = 0x2260; words[0] = 0x4127;
    words[2] = 6720; words[1] = 800; /* 8.4 V, 2 mV / 5 mOhm = 400 mA discharge. */
    power_read(&p);
    assert(p.result == POWER_OK && p.battery_mv == 8400 && p.battery_ua == -400000);
    assert(p.shunt_nv == 2000000 && p.estimated_percent == 100);
    assert(p.charger_valid && p.charge_enabled && p.charge_status_high);
    words[2] = 6000; words[1] = (uint16_t)-2000; /* 7.5 V, 1 A charge. */
    words[0xff] = 0x2261;
    power_read(&p);
    assert(p.result == POWER_OK && p.battery_mv == 7500 && p.battery_ua == 1000000);
    assert(p.shunt_nv == -5000000 && p.estimated_percent == 52);
    words[1] = 0x8000; power_read(&p); assert(p.battery_ua == 16384000);
    words[1] = 0x7fff; power_read(&p); assert(p.battery_ua == -16383500);
    words[1] = 0; words[2] = 0; power_read(&p);
    assert(p.result == POWER_OK && p.estimated_percent == -1 && p.battery_ua == 0);
    words[2] = 4800; power_read(&p); assert(p.estimated_percent == 0);
    words[2] = 7000; power_read(&p); assert(p.estimated_percent == -1);
    words[2] = 0x8000; power_read(&p); assert(p.result == POWER_READ_ERROR && p.estimated_percent == -1);
    words[2] = 6000;
    for (int reg = 0; reg < 3; reg++) {
        fail_reg = reg; power_read(&p);
        assert(p.result == POWER_READ_ERROR && p.estimated_percent == -1 && p.battery_mv == 0);
    }
    fail_reg = 0xfe; power_read(&p); assert(p.result == POWER_NO_RESPONSE);
    fail_reg = 0xff; power_read(&p); assert(p.result == POWER_READ_ERROR);
    fail_reg = -1; words[0xfe] = 0xffff;
    power_read(&p); assert(p.result == POWER_WRONG_DEVICE);
    words[0xfe] = 0x5449; words[0xff] = 0x1234;
    power_read(&p); assert(p.result == POWER_WRONG_DEVICE);
    words[0xff] = 0x2260; words[0] = 0x4120;
    power_read(&p); assert(p.result == POWER_NOT_READY && p.battery_mv == 0);
    words[0] = 0x4127; direction = 0xff;
    power_read(&p); assert(p.result == POWER_OK && !p.charger_valid);
    expander_ok = false;
    power_read(&p); assert(p.result == POWER_OK && !p.charger_valid);
    assert(strcmp(power_result_name(POWER_READ_ERROR), "read error") == 0);
    puts("power: identity, units, polarity, estimate limits, read failure tests PASS");
}
