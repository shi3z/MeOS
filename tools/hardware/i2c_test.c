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
static unsigned fast_ticks;
static unsigned clock_lows, scl_reads, stall_at, stretch_delays;
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
void hal_gpio_od_drive_low(int pin) { edge(pin); if(pin==expected_scl) clock_lows++; }
void hal_i2c_fast_tick(void) { fast_ticks++; }
void hal_delay_us(uint32_t us) { assert(us==4 || us==1); if(us==1)stretch_delays++; }
int hal_gpio_read(int pin)
{
    edge(pin);
    if (pin == expected_scl) return !stall_at || ++scl_reads<stall_at;
    assert(pos < nbits);
    return bits[pos++];
}
static void queue(int bit) { assert(nbits < 256); bits[nbits++] = bit; }
static void byte(unsigned b) { for (int i = 7; i >= 0; i--) queue((b >> i) & 1); }
static void begin(void) { pos = nbits = 0; locks = unlocks = 0; clock_lows=scl_reads=stretch_delays=0; stall_at=0; }
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
    begin(); queue(1); hal_i2c_init(); done();
    begin(); queue(0); queue(0); queue(0); byte(0x54); byte(0x49);
    assert(hal_i2c_sys_read_regs(0x41, 0xfe, b, 2) && b[0] == 0x54 && b[1] == 0x49); done();
    begin(); queue(0); hal_i2c_init(); assert(clock_lows==9); done(); /* Stuck SDA recovery clocks/STOP under one lock. */
    begin(); hal_i2c_set_pins(1, 0); done();
    expected_scl = 1; expected_sda = 0;
    begin(); queue(1); hal_i2c_init(); done();
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
    expected_scl=SYS_SCL;expected_sda=SYS_SDA;
    begin();stall_at=1;
    assert(hal_i2c_sys_read_regs_ex(0x32,0x10,b,1)==HAL_I2C_TIMEOUT);done();assert(stretch_delays==2000);
    begin();stall_at=10;
    assert(hal_i2c_sys_write_reg_ex(0x32,0x10,0)==HAL_I2C_TIMEOUT);done();
    begin();queue(0);queue(0);queue(0);stall_at=30;
    assert(hal_i2c_sys_read_regs_ex(0x32,0x10,b,1)==HAL_I2C_TIMEOUT);done();
    begin();queue(0);queue(0);queue(0);byte(0x55);stall_at=39;
    assert(hal_i2c_sys_read_regs_ex(0x32,0x10,b,1)==HAL_I2C_TIMEOUT);done();
    begin();queue(1);assert(hal_i2c_sys_read_regs_ex(0x32,0x10,b,1)==HAL_I2C_NACK);done();
    begin();assert(hal_i2c_sys_read_regs_ex(0x80,0,b,1)==HAL_I2C_INVALID);done();
    begin();assert(hal_i2c_sys_read_regs_ex(0x32,0,NULL,1)==HAL_I2C_INVALID);done();
    begin();queue(0);queue(0);queue(0);byte(0x66);
    assert(hal_i2c_sys_read_regs_ex(0x32,0x10,b,1)==HAL_I2C_OK && b[0]==0x66);done();
    begin();queue(0);queue(0);queue(0);queue(0);byte(8);
    fast_ticks=0;assert(hal_i2c_read_reg16_fast(0x55,0x10,b,1) && b[0]==8);done();assert(fast_ticks>0);
    begin();queue(1);assert(!hal_i2c_read_reg16_fast(0x55,0x10,b,1));done();
    begin();stall_at=1;assert(!hal_i2c_read_reg16_fast(0x55,0x10,b,1));done();
    fast_ticks=0;begin();queue(0);queue(0);queue(0);byte(0x66);
    assert(hal_i2c_sys_read_regs_ex(0x32,0x10,b,1)==HAL_I2C_OK && b[0]==0x66);done();assert(!fast_ticks);
    hal_i2c_stats_t stats;hal_i2c_get_stats(&stats);
    assert(stats.timeout==5 && stats.invalid==2 && stats.nack>=2 && stats.ok>0);
    puts("i2c: boot/task locks, RMW, system/accessory routing, legacy APIs, NACK tests PASS");
}
