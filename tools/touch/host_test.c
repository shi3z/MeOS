/* Register-level driver regression tests: no ESP-IDF or hardware needed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <m5os/hal_i2c.h>
#include "../../drivers/tab5kbd/tab5touch.h"

static uint8_t st[256], gt[65536];
static bool have_st, have_gt, fail_read;
static uint16_t fail_reg;
static unsigned writes, wakes;

void kprintf(const char *fmt, ...) { (void)fmt; }
void hal_gpio_config_output(int pin, int level) { (void)pin; (void)level; wakes++; }
void hal_gpio_config_input_pullup(int pin) { (void)pin; }
void hal_gpio_write(int pin, int level) { (void)pin; (void)level; }
void hal_delay_us(uint32_t us) { (void)us; }

bool hal_i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t *buf, size_t len)
{
    if (fail_read || (fail_reg && reg == fail_reg)) return false;
    if (addr == 0x55 && have_st) {
        assert(reg + len <= sizeof st);
        memcpy(buf, st + reg, len); return true;
    }
    if (addr == 0x14 && have_gt) {
        assert(reg + len <= sizeof gt);
        memcpy(buf, gt + reg, len); return true;
    }
    return false;
}
bool hal_i2c_write_reg16(uint8_t addr, uint16_t reg, uint8_t val)
{
    assert(addr == 0x14); /* ST712x must never receive GT911 acknowledgement writes. */
    writes++;
    if (!have_gt) return false;
    gt[reg] = val; return true;
}
static void reset(void)
{
    memset(st, 0, sizeof st); memset(gt, 0, sizeof gt);
    have_st = have_gt = fail_read = false; fail_reg = 0; writes = wakes = 0;
}
static void st_info(int x, int y, int slots)
{
    have_st = true; st[0] = 1;
    st[5] = x >> 8; st[6] = x; st[7] = y >> 8; st[8] = y; st[9] = slots;
}
static void point(int slot, int x, int y)
{
    uint8_t *p = st + 0x14 + slot * 7;
    p[0] = 0x80 | (x >> 8); p[1] = x; p[2] = y >> 8; p[3] = y;
    st[0x10] = 8;
}
static void gt_info(int x, int y)
{
    have_gt = true; memcpy(gt + 0x8140, "911", 4);
    gt[0x8048] = x; gt[0x8049] = x >> 8; gt[0x804a] = y; gt[0x804b] = y >> 8;
}
int main(void)
{
    int x = -1, y = -1;
    uint32_t frames, errors; int rx, ry, mx, my;
    reset(); assert(!tab5_touch_init()); assert(!tab5_touch_read(&x, &y));
    reset(); st_info(720, 1280, 10); assert(tab5_touch_init());
    assert(writes == 0 && wakes == 0 && tab5_touch_map() == 1);
    assert(!tab5_touch_read(&x, &y));
    point(9, 123, 456); /* Only the final slot is occupied. */
    assert(tab5_touch_read(&x, &y) && x == 823 && y == 123);
    tab5_touch_stats(&frames, &errors, &rx, &ry, &mx, &my);
    assert(frames == 1 && errors == 0 && rx == 123 && ry == 456 && mx == 720 && my == 1280);
    assert(tab5_touch_read(&x, &y)); /* Held finger. */
    st[0x14 + 9 * 7] = 0; assert(!tab5_touch_read(&x, &y)); /* Release. */
    point(0, 720, 100); assert(!tab5_touch_read(&x, &y)); /* Malformed coordinate. */
    point(0, 0, 0); assert(tab5_touch_read(&x, &y) && x == 1279 && y == 0);
    point(0, 719, 1279); assert(tab5_touch_read(&x, &y) && x == 0 && y == 719);
    fail_read = true; assert(!tab5_touch_read(&x, &y)); fail_read = false;
    fail_reg = 0x14; assert(!tab5_touch_read(&x, &y)); fail_reg = 0;
    st[0x10] = 0; assert(!tab5_touch_read(&x, &y));
    assert(writes == 0 && wakes == 0);
    reset(); st_info(1280, 720, 1); st[0] = 3; assert(tab5_touch_init());
    point(0, 1000, 600); assert(tab5_touch_read(&x, &y) && x == 1000 && y == 600);
    tab5_touch_set_map(3); assert(tab5_touch_read(&x, &y) && x == 279 && y == 119);
    reset(); st_info(720, 1280, 11); assert(!tab5_touch_init());
    reset(); st_info(0, 0, 10); assert(!tab5_touch_init());
    /* Existing GT911 protocol, held-frame semantics and little-endian decoding. */
    reset(); gt_info(720, 1280); assert(tab5_touch_init());
    assert(wakes == 1 && tab5_touch_map() == 1);
    gt[0x814e] = 0x81; gt[0x8150] = 123; gt[0x8152] = 0xc8; gt[0x8153] = 1;
    unsigned before = writes;
    assert(tab5_touch_read(&x, &y) && x == 823 && y == 123);
    assert(writes == before + 1 && gt[0x814e] == 0);
    assert(tab5_touch_read(&x, &y) && x == 823 && y == 123);
    gt[0x814e] = 0x80; assert(!tab5_touch_read(&x, &y));
    reset(); gt_info(1280, 720); assert(tab5_touch_init() && tab5_touch_map() == 0);
    /* Bad ST info falls back to GT911; failed reinit clears previously detected state. */
    st_info(0, 0, 0); assert(tab5_touch_init());
    reset(); assert(!tab5_touch_init()); assert(!tab5_touch_read(&x, &y));
    /* Sparse multi-touch, capacity, legacy first-point and per-contact validation. */
    tab5_touch_point_t pts[TAB5_TOUCH_MAX_POINTS];
    reset(); st_info(720, 1280, 10); assert(tab5_touch_init());
    point(2, 650, 1200); point(9, 680, 600);
    assert(tab5_touch_read_points(pts, 10) == 2);
    assert(pts[0].id == 2 && pts[0].x == 79 && pts[0].y == 650);
    assert(pts[1].id == 9 && pts[1].x == 679 && pts[1].y == 680);
    assert(tab5_touch_read_points(pts, 1) == 1 && pts[0].id == 2);
    assert(tab5_touch_read(&x, &y) && x == 79 && y == 650);
    st[0x14 + 2 * 7] = 0;
    assert(tab5_touch_read_points(pts, 10) == 1 && pts[0].id == 9);
    point(2, 720, 0); assert(tab5_touch_read_points(pts, 10) == 1 && pts[0].id == 9);
    fail_reg = 0x14; assert(tab5_touch_read_points(pts, 10) == 0); fail_reg = 0;
    assert(!tab5_touch_read_points(NULL, 10) && !tab5_touch_read_points(pts, 0));
    /* GT911 packed eight-byte records, little endian, held snapshots and ACK. */
    reset(); gt_info(1280, 720); assert(tab5_touch_init());
    gt[0x814e] = 0x82;
    gt[0x814f] = 3; gt[0x8150] = 70; gt[0x8152] = 0x8a; gt[0x8153] = 2;
    gt[0x8157] = 7; gt[0x8158] = 0x9e; gt[0x8159] = 2;
    gt[0x815a] = 0xa8; gt[0x815b] = 2;
    before = writes;
    assert(tab5_touch_read_points(pts, 10) == 2 && writes == before + 1);
    assert(pts[0].id == 3 && pts[0].x == 70 && pts[0].y == 650);
    assert(pts[1].id == 7 && pts[1].x == 670 && pts[1].y == 680);
    assert(tab5_touch_read_points(pts, 10) == 2); /* no fresh frame */
    gt[0x814e] = 0x81; memcpy(gt + 0x814f, gt + 0x8157, 8);
    assert(tab5_touch_read_points(pts, 10) == 1 && pts[0].id == 7);
    gt[0x814e] = 0x82; fail_reg = 0x814f;
    assert(tab5_touch_read_points(pts, 10) == 0 && gt[0x814e] == 0); fail_reg = 0;
    gt[0x814e] = 0x86; assert(!tab5_touch_read_points(pts, 10));
    gt[0x814e] = 0x80; assert(!tab5_touch_read_points(pts, 10));
    puts("touch: GT911 / ST712x host regression tests PASS");
    return 0;
}
