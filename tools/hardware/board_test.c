/* Exercise the actual board startup path with transient/permanent reset NACKs. */
#include <assert.h>
#include <stdio.h>
#include <m5os/board.h>
#include <m5os/hal_i2c.h>
#include <m5os/hal_gpio.h>
#include <m5os/kprintf.h>
static unsigned attempts[2], failures[2], bus_inits, writes;
void hal_i2c_init(void) { bus_inits++; }
bool hal_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value) {
    assert(addr == 0x43 || addr == 0x44); writes++;
    if (reg != 1) return true;
    assert(value == 0xff);
    unsigned i = addr - 0x43;
    return ++attempts[i] > failures[i];
}
bool hal_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *value) {
    (void)reg; *value = 0; return addr == 0x43 || addr == 0x44;
}
bool hal_i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t *value, size_t len) {
    (void)addr; (void)reg; (void)value; (void)len; return false;
}
/* The fixed-system-bus helpers the extension-port 5 V switch uses: a fake expander 1 output register. */
static uint8_t fake_out = 0x66;
static bool fail_read, fail_update;
bool hal_i2c_sys_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
    assert(addr == 0x43 && reg == 0x05 && len == 1);
    if (fail_read) return false;
    buf[0] = fake_out; return true;
}
bool hal_i2c_sys_update_reg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value) {
    assert(addr == 0x43 && reg == 0x05);
    if (fail_update) return false;
    fake_out = (uint8_t)((fake_out & ~mask) | (value & mask)); return true;
}
void hal_delay_us(uint32_t us) { assert(us == 10000 || us == 50000 || us == 100000); }
void hal_gpio_config_output(int pin, int level) { (void)pin; (void)level; }
void kprintf(const char *fmt, ...) { (void)fmt; }
static void reset(unsigned a, unsigned b) {
    attempts[0] = attempts[1] = bus_inits = writes = 0;
    failures[0] = a; failures[1] = b;
}
int main(void) {
    reset(0, 0); assert(board_init());
    assert(attempts[0] == 1 && attempts[1] == 1 && bus_inits == 1);
    reset(2, 1); assert(board_init());
    assert(attempts[0] == 3 && attempts[1] == 2 && bus_inits == 4);
    reset(3, 0); assert(!board_init());
    assert(attempts[0] == 3 && attempts[1] == 0 && writes == 3 && bus_inits == 4);
    reset(0, 3); assert(!board_init());
    assert(attempts[0] == 1 && attempts[1] == 3 && bus_inits == 4);
    /* extension-port 5 V: only P2 of expander 1 changes, the previous state is reported, failures change nothing */
    {
        bool was = false;
        fake_out = 0x66;
        assert(board_ext5v_set(false, &was) && was && fake_out == 0x62);     /* 0110 0110 -> 0110 0010 */
        assert(board_ext5v_set(false, &was) && !was && fake_out == 0x62);
        assert(board_ext5v_set(true, NULL) && fake_out == 0x66);
        assert(board_ext5v_set(true, &was) && was && fake_out == 0x66);
        fail_read = true;  was = false; assert(!board_ext5v_set(false, &was) && fake_out == 0x66); fail_read = false;
        fail_update = true; assert(!board_ext5v_set(false, NULL) && fake_out == 0x66); fail_update = false;
        board_backlight(200); assert(board_backlight_get() == 200);
        board_backlight(0);   assert(board_backlight_get() == 0);
    }
    puts("Tab5 board: bounded reset retries, recovery, permanent NACK failure, 5 V switch and backlight state PASS");
}
