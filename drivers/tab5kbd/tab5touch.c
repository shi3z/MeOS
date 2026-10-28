/*
 * Tab5 capacitive touch: GT911 (Goodix, ILI9881C units) and ST712x (ST7121/ST7123 units).
 *
 * The GT911 sits on the system I2C bus (GPIO 31/32) at address 0x14; the board init
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
#include <m5os/tab5.h>

/* ST7121/ST7123 protocol reference (register layout, not the GT911 handshake):
 * https://github.com/m5stack/M5GFX/blob/master/src/lgfx/v1/platforms/esp32p4/Touch_ST7123.inl
 * Board/display reset is already complete. ST712x needs neither the GT911 INT wake pulse
 * nor a status acknowledgement write. Its coordinates and resolution are big-endian. */
#define ST_INFO         0x0005
#define ST_STATUS       0x0010
#define ST_POINTS       0x0014
#define ST_MAX_TOUCHES  10
#define ST_POINT_BYTES  7

typedef enum { TOUCH_NONE, TOUCH_GT911, TOUCH_ST712X } touch_kind_t;
static touch_kind_t s_kind;
static uint8_t s_touch_slots;

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
    s_present = s_pressed = false;
    s_kind = TOUCH_NONE;
    s_map = 1;
    s_frames = s_errors = 0;
    s_raw_x = s_raw_y = s_max_x = s_max_y = 0;
    s_touch_slots = 0;

    uint8_t fw = 0, info[5] = { 0 };
    if (hal_i2c_read_reg16(TAB5_ST7123_TOUCH_ADDR, TAB5_ST7123_FW_REG, &fw, 1) &&
        hal_i2c_read_reg16(TAB5_ST7123_TOUCH_ADDR, ST_INFO, info, sizeof info)) {
        int mx = (info[0] << 8) | info[1], my = (info[2] << 8) | info[3];
        /* Validate the geometry before accepting an unrelated/garbled bus response. */
        if (((mx == DISPLAY_HEIGHT && my == DISPLAY_WIDTH) ||
             (mx == DISPLAY_WIDTH && my == DISPLAY_HEIGHT)) &&
            info[4] > 0 && info[4] <= ST_MAX_TOUCHES) {
            s_max_x = mx; s_max_y = my; s_touch_slots = info[4];
            if (mx > my) s_map = 0;
            s_kind = TOUCH_ST712X;
            s_present = true;
            kprintf("touch: ST712x at 0x%02x firmware %u, resolution %d x %d, %u slots, map %d\n",
                    TAB5_ST7123_TOUCH_ADDR, fw, mx, my, s_touch_slots, s_map);
            return true;
        }
    }

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
    s_kind = TOUCH_GT911;
    return true;
}

static bool read_st712x(int *x, int *y)
{
    uint8_t status = 0, points[ST_MAX_TOUCHES * ST_POINT_BYTES];
    s_pressed = false; /* A missing/failed report must not leave a repeating key held. */
    if (!hal_i2c_read_reg16(TAB5_ST7123_TOUCH_ADDR, ST_STATUS, &status, 1)) {
        s_errors++; return false;
    }
    if (!(status & 0x08)) return false; /* with_coord: other reports contain no touch */
    if (!hal_i2c_read_reg16(TAB5_ST7123_TOUCH_ADDR, ST_POINTS, points,
                            (size_t)s_touch_slots * ST_POINT_BYTES)) {
        s_errors++; return false;
    }
    s_frames++;
    /* Slots are sparse: slot zero need not be the active finger. Keep the first valid one. */
    for (unsigned i = 0; i < s_touch_slots; i++) {
        const uint8_t *p = points + i * ST_POINT_BYTES;
        if (!(p[0] & 0x80)) continue;
        int rx = ((p[0] & 0x3f) << 8) | p[1], ry = (p[2] << 8) | p[3];
        if (rx >= s_max_x || ry >= s_max_y) { s_errors++; continue; }
        s_raw_x = rx; s_raw_y = ry;
        map_point(rx, ry, &s_x, &s_y);
        s_pressed = true;
        *x = s_x; *y = s_y;
        break;
    }
    return s_pressed;
}

bool tab5_touch_read(int *x, int *y)
{
    if (!s_present) return false;
    if (s_kind == TOUCH_ST712X) return read_st712x(x, y);
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
const char *tab5_touch_name(void)
{
    return s_kind == TOUCH_GT911 ? "GT911" : s_kind == TOUCH_ST712X ? "ST712x" : "none";
}
int  tab5_touch_map(void) { return s_map; }
void tab5_touch_stats(uint32_t *frames, uint32_t *errors, int *raw_x, int *raw_y, int *max_x, int *max_y)
{
    *frames = s_frames; *errors = s_errors; *raw_x = s_raw_x; *raw_y = s_raw_y; *max_x = s_max_x; *max_y = s_max_y;
}
