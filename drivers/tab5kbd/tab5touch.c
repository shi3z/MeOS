/*
 * GT911 capacitive touch controller (Goodix), as fitted to the ILI9881C revision of the Tab5.
 *
 * The controller sits on the system I2C bus (GPIO 31/32) at address 0x14; the board init
 * (hal/esp32p4/board_tab5.c) has already pulsed its reset line through the I/O expander.
 * Registers are 16-bit (big-endian number, then data):
 *   0x8140  product id, 4 ASCII bytes ("911")
 *   0x8048  X resolution (little-endian 16), 0x804A Y resolution: the frame the coordinates are in
 *   0x814E  status: bit 7 = a new frame is ready, bits 0-3 = number of fingers (0 when all lifted)
 *   0x814F  first point: track id; 0x8150 x lo, x hi, y lo, y hi, size lo, size hi, reserved (8 bytes per point)
 * After a frame has been read the host writes 0 to 0x814E, or the controller stops producing new ones.
 * Only the first finger is used.
 *
 * The 16-bit register helpers always use the system bus pins, whichever bus the 8-bit ones are
 * switched to (the Tab5 Keyboard accessory moves those to Ext.Port1), so the two keyboards and the
 * touch panel can coexist.
 */
#include "tab5touch.h"
#include <m5os/hal_i2c.h>
#include <m5os/hal_gpio.h>
#include <m5os/kprintf.h>
#include <m5os/display.h>

#define GT911_ADDR      0x14
#define PIN_INT         23              /* GT911 INT: an output of the controller, also its wake-up input */
#define REG_PRODUCT     0x8140
#define REG_XRES        0x8048
#define REG_STATUS      0x814E
#define REG_POINT0      0x8150

static bool     s_present;
static bool     s_pressed;
static int      s_x, s_y;                    /* logical */
static int      s_raw_x, s_raw_y, s_max_x, s_max_y;
static int      s_map = 1;
static uint32_t s_frames, s_errors;

/* raw -> logical landscape.  The panel is physically portrait (720 wide, 1280 tall) and the display driver
 * stores logical (x, y) at physical column y, row 1279 - x; map 1 is the inverse of that, which is what a
 * controller configured in the panel's own frame reports.  The others are there for the other possible
 * configurations and can be selected from the shell (`osk map N`) during bring-up. */
static void map_point(int rx, int ry, int *x, int *y)
{
    switch (s_map) {
    default:
    case 0: *x = rx;                       *y = ry;                       break;
    case 1: *x = DISPLAY_WIDTH - 1 - ry;   *y = rx;                       break;
    case 2: *x = ry;                       *y = DISPLAY_HEIGHT - 1 - rx;  break;
    case 3: *x = DISPLAY_WIDTH - 1 - rx;   *y = DISPLAY_HEIGHT - 1 - ry;  break;
    }
    if (*x < 0) *x = 0;
    if (*x >= DISPLAY_WIDTH) *x = DISPLAY_WIDTH - 1;
    if (*y < 0) *y = 0;
    if (*y >= DISPLAY_HEIGHT) *y = DISPLAY_HEIGHT - 1;
}

/* Wake the controller the way M5GFX (Touch_GT911::wakeup) does: INT held low for 58 ms by the host, then
 * released; INT then goes back to being the controller's output, read as an input with pull-up. */
static void wake(void)
{
    hal_gpio_config_output(PIN_INT, 0);
    hal_delay_us(58000);
    hal_gpio_write(PIN_INT, 1);
    hal_delay_us(2000);
    hal_gpio_config_input_pullup(PIN_INT);
}

bool tab5_touch_init(void)
{
    uint8_t id[4] = { 0 }, res[4] = { 0 };
    hal_i2c_write_reg16(GT911_ADDR, REG_STATUS, 0);
    wake();
    if (!hal_i2c_read_reg16(GT911_ADDR, REG_PRODUCT, id, 4) || id[0] != '9') {
        kprintf("touch: no GT911 at 0x%02x (id %02x %02x %02x %02x)\n", GT911_ADDR, id[0], id[1], id[2], id[3]);
        return false;
    }
    hal_i2c_read_reg16(GT911_ADDR, REG_XRES, res, 4);
    s_max_x = res[0] | (res[1] << 8); s_max_y = res[2] | (res[3] << 8);
    /* A controller configured in landscape (x range ~1280) reports logical coordinates already. */
    if (s_max_x > s_max_y) s_map = 0;
    kprintf("touch: GT911 \"%c%c%c%c\" resolution %d x %d, map %d\n", id[0], id[1], id[2], id[3] ? id[3] : ' ', s_max_x, s_max_y, s_map);
    hal_i2c_write_reg16(GT911_ADDR, REG_STATUS, 0);
    s_present = true;
    return true;
}

bool tab5_touch_read(int *x, int *y)
{
    if (!s_present) return false;
    uint8_t st = 0;
    if (!hal_i2c_read_reg16(GT911_ADDR, REG_STATUS, &st, 1)) { s_errors++; return false; }
    if (st & 0x80) {
        int n = st & 0x0F;
        if (n > 0) {
            uint8_t p[6];
            if (hal_i2c_read_reg16(GT911_ADDR, REG_POINT0, p, sizeof p)) {
                s_raw_x = p[0] | (p[1] << 8); s_raw_y = p[2] | (p[3] << 8);
                map_point(s_raw_x, s_raw_y, &s_x, &s_y);
                s_pressed = true;
            } else s_errors++;
        } else s_pressed = false;
        s_frames++;
        hal_i2c_write_reg16(GT911_ADDR, REG_STATUS, 0);
    }
    if (s_pressed) { *x = s_x; *y = s_y; }
    return s_pressed;
}

void tab5_touch_set_map(int map) { s_map = map & 3; }
int  tab5_touch_map(void) { return s_map; }
void tab5_touch_stats(uint32_t *frames, uint32_t *errors, int *raw_x, int *raw_y, int *max_x, int *max_y)
{
    *frames = s_frames; *errors = s_errors; *raw_x = s_raw_x; *raw_y = s_raw_y; *max_x = s_max_x; *max_y = s_max_y;
}
