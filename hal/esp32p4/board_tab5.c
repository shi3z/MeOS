/*
 * M5Stack Tab5 board bring-up: the two PI4IOE5V6408 I/O expanders on the system I2C bus.
 *
 * Why this exists: on the Tab5 the panel reset, touch reset, the USB host's 5 V switch, the Wi-Fi
 * module power and the charger are NOT wired to the SoC but to I/O expanders.  Until the expanders
 * are configured, the keyboard plugged into the USB-A port gets no power and the panel stays in reset.
 *
 * Register map and pin assignments are taken from M5Stack's board support package
 * (M5Tab5-UserDemo, components/m5stack_tab5/m5stack_tab5.c: bsp_io_expander_pi4ioe_init); the values
 * below are the same, with the bit meanings spelled out.
 */
#include <m5os/board.h>
#include <m5os/hal_i2c.h>
#include <m5os/hal_gpio.h>
#include <m5os/kprintf.h>
#include <m5os/tab5.h>

#define EXP1_ADDR   0x43            /* address pin low  */
#define EXP2_ADDR   0x44            /* address pin high */

/* PI4IOE5V6408 registers (datasheet register map; same numbers as in the BSP) */
#define REG_CHIP_RESET  0x01
#define REG_IO_DIR      0x03        /* 1 = output */
#define REG_OUT_SET     0x05
#define REG_OUT_H_IM    0x07        /* 1 = output stage high-impedance */
#define REG_IN_DEF      0x09
#define REG_PULL_EN     0x0B
#define REG_PULL_SEL    0x0D        /* 1 = pull-up, 0 = pull-down */
#define REG_IN_STATE    0x0F
#define REG_INT_MASK    0x11

/* Expander 1 bits */
#define EXP1_LCD_RST_BIT   4        /* LCD reset: only ever pulled low or released as input (see below) */

/* Expander 2 bits: P0 Wi-Fi power, P3 USB 5 V enable, P7 charger enable */
#define EXP2_OUT_INIT      0x09     /* P0 and P3 high */

#define PANEL_RESET_US     10000u
#define PANEL_RELEASE_US   50000u
#define TOUCH_RST_BIT      5           /* expander 1 P5: GT911 reset (shares the sequence below with LCD_RST) */
#define RESET_PULSE_US     100000u     /* vendor BSP: LCD_RST low for 100 ms, then 100 ms before talking to the panel */

static tab5_panel_t s_panel = TAB5_PANEL_UNKNOWN;

static bool exp_write(uint8_t addr, uint8_t reg, uint8_t v) { return hal_i2c_write_reg(addr, reg, v); }
static bool exp_reset(uint8_t addr)
{
    for (unsigned attempt=0;attempt<3;attempt++) {
        if (exp_write(addr,REG_CHIP_RESET,0xff)) return true;
        hal_delay_us(10000); hal_i2c_init();
    }
    return false;
}

bool board_init(void)
{
    hal_i2c_init();

    /* ---- expander 1: LCD_RST (P4) and friends ---- */
    if (!exp_reset(EXP1_ADDR)) {
        kprintf("tab5: expander 1 (0x%02x) does not answer\n", EXP1_ADDR);
        return false;
    }
    /* After reset every pin is high-impedance with pull-down.  The panel's reset line has no pull-up
     * to its own 1.8 V rail, so the expander must never drive it high (3.3 V): hold the latch at 0,
     * make P4 an output to pull reset low, then turn it back into an input with pull-up to release. */
    exp_write(EXP1_ADDR, REG_PULL_SEL, 0x7F);
    exp_write(EXP1_ADDR, REG_PULL_EN,  0x7F);
    exp_write(EXP1_ADDR, REG_OUT_SET,  0x66);          /* P4 latch = 0 */
    exp_write(EXP1_ADDR, REG_OUT_H_IM, 0x00);
    exp_write(EXP1_ADDR, REG_IO_DIR,   0x7F);          /* P4 output: reset asserted */
    hal_delay_us(PANEL_RESET_US);
    exp_write(EXP1_ADDR, REG_IO_DIR,   0x6F);          /* P4 input + pull-up: reset released */
    hal_delay_us(PANEL_RELEASE_US);

    /* The vendor BSP repeats the reset right before it starts the display (bsp_reset_tp): LCD_RST and
     * TOUCH_RST low together for 100 ms, then both released, 100 ms settle.  A 10 ms pulse is what the
     * first reset above gives; this one is the length the panel is known to start up reliably with. */
    uint8_t v;
    hal_i2c_read_reg(EXP1_ADDR, REG_OUT_SET, &v);
    exp_write(EXP1_ADDR, REG_OUT_SET, (uint8_t)(v & ~((1u << EXP1_LCD_RST_BIT) | (1u << TOUCH_RST_BIT))));
    hal_i2c_read_reg(EXP1_ADDR, REG_IO_DIR, &v);
    exp_write(EXP1_ADDR, REG_IO_DIR, (uint8_t)(v | (1u << EXP1_LCD_RST_BIT)));          /* LCD_RST output: low */
    hal_delay_us(RESET_PULSE_US);
    hal_i2c_read_reg(EXP1_ADDR, REG_OUT_SET, &v);
    exp_write(EXP1_ADDR, REG_OUT_SET, (uint8_t)((v & ~(1u << EXP1_LCD_RST_BIT)) | (1u << TOUCH_RST_BIT)));
    hal_i2c_read_reg(EXP1_ADDR, REG_IO_DIR, &v);
    exp_write(EXP1_ADDR, REG_IO_DIR, (uint8_t)(v & ~(1u << EXP1_LCD_RST_BIT)));         /* LCD_RST input + pull-up: released */
    hal_delay_us(RESET_PULSE_US);

    /* ---- expander 2: power switches (USB 5 V for the keyboard, Wi-Fi, charger) ---- */
    if (!exp_reset(EXP2_ADDR)) {
        kprintf("tab5: expander 2 (0x%02x) does not answer\n", EXP2_ADDR);
        return false;
    }
    exp_write(EXP2_ADDR, REG_IO_DIR,   0xB9);
    exp_write(EXP2_ADDR, REG_OUT_H_IM, 0x06);
    exp_write(EXP2_ADDR, REG_PULL_SEL, 0xB9);
    exp_write(EXP2_ADDR, REG_PULL_EN,  0xF9);
    exp_write(EXP2_ADDR, REG_IN_DEF,   0x40);
    exp_write(EXP2_ADDR, REG_INT_MASK, 0xBF);
    exp_write(EXP2_ADDR, REG_OUT_SET,  EXP2_OUT_INIT);
    {   /* bring-up aid: read the expanders back so a wrong wiring assumption shows up in the log */
        uint8_t o1 = 0, d1 = 0, i1 = 0, o2 = 0, d2 = 0, i2 = 0;
        hal_i2c_read_reg(EXP1_ADDR, REG_OUT_SET, &o1); hal_i2c_read_reg(EXP1_ADDR, REG_IO_DIR, &d1); hal_i2c_read_reg(EXP1_ADDR, REG_IN_STATE, &i1);
        hal_i2c_read_reg(EXP2_ADDR, REG_OUT_SET, &o2); hal_i2c_read_reg(EXP2_ADDR, REG_IO_DIR, &d2); hal_i2c_read_reg(EXP2_ADDR, REG_IN_STATE, &i2);
        kprintf("tab5: expander1 out=%02x dir=%02x in=%02x | expander2 out=%02x dir=%02x in=%02x\n", o1, d1, i1, o2, d2, i2);
    }

    /* Which panel is fitted?  The two hardware revisions carry different touch controllers, and the
     * vendor BSP tells them apart by probing the I2C bus: GT911 (0x14) -> ILI9881C, 0x55 -> ST7123/ST7121. */
    uint8_t dummy;
    if (hal_i2c_read_reg(TAB5_GT911_ADDR_ALT, 0, &dummy))      s_panel = TAB5_PANEL_ILI9881C;
    else if (hal_i2c_read_reg(TAB5_ST7123_TOUCH_ADDR, 0, &dummy)) {
        uint8_t fw = 0;
        s_panel = TAB5_PANEL_ST7123;                           /* BSP: an unreadable version also means ST7123 */
        if (hal_i2c_read_reg16(TAB5_ST7123_TOUCH_ADDR, TAB5_ST7123_FW_REG, &fw, 1) && fw == 1) s_panel = TAB5_PANEL_ST7121;
        kprintf("tab5: touch controller 0x%02x firmware version %u\n", TAB5_ST7123_TOUCH_ADDR, fw);
    } else                                                     s_panel = TAB5_PANEL_ILI9881C;   /* BSP default */
    return true;
}

tab5_panel_t tab5_panel(void) { return s_panel; }

/* The backlight driver is switched by GPIO22 (the vendor BSP dims it with LEDC PWM).  Plain on/off is
 * enough here: any non-zero level means on.  PWM dimming can replace this without touching callers. */
#define BACKLIGHT_PIN  22

static uint8_t s_backlight;

void board_backlight(uint8_t level)
{
    s_backlight = level;
    hal_gpio_config_output(BACKLIGHT_PIN, level ? 1 : 0);
}

uint8_t board_backlight_get(void) { return s_backlight; }

/* Expander 1, P2: the 5 V pin of the extension port (the same switch the vendor BSP calls bsp_set_ext_5v_en).
 * The USB-A host port's 5 V is expander 2, P3 - a different switch, deliberately left alone. */
#define EXP1_EXT5V_BIT  2

bool board_ext5v_set(bool on, bool *was_on)
{
    uint8_t v;
    if (!hal_i2c_sys_read_regs(EXP1_ADDR, REG_OUT_SET, &v, 1)) return false;
    if (was_on) *was_on = (v >> EXP1_EXT5V_BIT) & 1u;
    return hal_i2c_sys_update_reg(EXP1_ADDR, REG_OUT_SET, 1u << EXP1_EXT5V_BIT, on ? 1u << EXP1_EXT5V_BIT : 0);
}
