/* Exercise the production bit-bang HAL through mocked GPIO and mutex boundaries. */
#include <assert.h>
#include <stdio.h>
#include <m5os/hal_i2c.h>
#include <m5os/board_config.h>
#include <m5os/hal_gpio.h>
#include <m5os/mutex.h>

#if M5OS_BOARD_TAB5
#define SYS_SCL 32
#define SYS_SDA 31
#elif M5OS_BOARD_CARDPUTER
#define SYS_SCL 9
#define SYS_SDA 8
#else
#define SYS_SCL 11
#define SYS_SDA 12
#endif
static bool running;
static task_t task;
static int depth, locks, unlocks, expected_scl = SYS_SCL, expected_sda = SYS_SDA;
static int bits[256], nbits, pos;
task_t *task_current(void) { return running ? &task : NULL; }
void mutex_lock(mutex_t *m) { (void)m; assert(depth == 0); depth++; locks++; }
void mutex_unlock(mutex_t *m) { (void)m; assert(depth == 1); depth--; unlocks++; }
static void edge(int pin)
{
    assert(pin == expected_scl || pin == expected_sda);
    assert(depth == (running ? 1 : 0));
}
void hal_gpio_config_open_drain(int pin) { edge(pin); }
void hal_gpio_od_release(int pin) { edge(pin); }
void hal_gpio_od_drive_low(int pin) { edge(pin); }
void hal_delay_us(uint32_t us) { (void)us; }
int hal_gpio_read(int pin)
{
    edge(pin);
    if (pin == expected_scl) return 1;
    assert(pos < nbits);
    return bits[pos++];
}
static void queue(int bit) { assert(nbits < 256); bits[nbits++] = bit; }
static void byte(unsigned b) { for (int i = 7; i >= 0; i--) queue((b >> i) & 1); }
static void begin(void) { pos = nbits = 0; locks = unlocks = 0; }
static void done(void)
{
    assert(pos == nbits && depth == 0);
    assert(locks == (running ? 1 : 0) && locks == unlocks);
}
int main(void)
{
    uint8_t b[2];
    begin(); queue(0); queue(0); queue(0);
    assert(hal_i2c_write_reg(0x43, 5, 0x66)); done(); /* Before scheduler: no mutex. */
    running = true;
    begin(); hal_i2c_init(); done();
    begin(); queue(0); queue(0); queue(0); byte(0x54); byte(0x49);
    assert(hal_i2c_sys_read_regs(0x41, 0xfe, b, 2) && b[0] == 0x54 && b[1] == 0x49); done();
    begin(); hal_i2c_set_pins(1, 0); done();
    expected_scl = 1; expected_sda = 0;
    begin(); hal_i2c_init(); done();
    begin(); queue(0); queue(0); queue(0); byte(0x12);
    assert(hal_i2c_read_reg(0x6d, 0xfe, b) && b[0] == 0x12); done();
    begin(); queue(0); queue(0); queue(0);
    assert(hal_i2c_write_reg(0x6d, 0x10, 1)); done();
    begin(); queue(0); queue(0); queue(0); byte(0xaa); queue(0); queue(0); queue(0);
    assert(hal_i2c_bit_on(0x6d, 0x10, 1)); done(); /* RMW uses ONE lock, no recursive locking. */
    begin(); queue(0); queue(0); queue(0); byte(0xaa); queue(0); queue(0); queue(0);
    assert(hal_i2c_bit_off(0x6d, 0x10, 1)); done();
    expected_scl = SYS_SCL; expected_sda = SYS_SDA;
    begin(); queue(0); queue(0); queue(0); queue(0); byte(0x81);
    assert(hal_i2c_read_reg16(0x14, 0x814e, b, 1) && b[0] == 0x81); done();
    begin(); queue(0); queue(0); queue(0); queue(0);
    assert(hal_i2c_write_reg16(0x14, 0x814e, 0)); done();
    begin(); queue(0); queue(0); queue(0); byte(0x40);
    assert(hal_i2c_sys_read_regs(0x44, 0xf, b, 1) && b[0] == 0x40); done();
    begin(); queue(1);
    assert(!hal_i2c_sys_read_regs(0x41, 0xfe, b, 2)); done(); /* NACK still releases the lock. */
    begin(); queue(0); queue(0); queue(0);
    assert(hal_i2c_sys_write_reg(0x10, 0x19, 0x24)); done();
    begin(); queue(0); queue(0); queue(0); byte(0x64); queue(0); queue(0); queue(0);
    assert(hal_i2c_sys_update_reg(0x43, 5, 2, 2)); done(); /* Fixed bus, one lock, preserves LCD reset. */
    begin(); queue(1); assert(!hal_i2c_sys_update_reg(0x43, 5, 2, 0)); done();
    expected_scl = 1; expected_sda = 0;
    begin(); queue(0); queue(0); queue(0); byte(0x34);
    assert(hal_i2c_read_reg(0x6d, 0xfe, b) && b[0] == 0x34); done(); /* System access did not switch bus. */
    puts("i2c: boot/task locks, RMW, system/accessory routing, legacy APIs, NACK tests PASS");
}
