/*
 * Keyboard for the Tab5.  Two kinds can be attached, and keyboard_start() looks for them in this order:
 *
 *  1. M5Stack's "Tab5 Keyboard" accessory: plugs into Ext.Port1 and talks I2C (SDA = G0, SCL = G1,
 *     address 0x6D, INT on G50).  A small microcontroller scans the key matrix and keeps a queue of
 *     events; we put it into its HID mode and drain the queue.  Register map and mode numbers from
 *     M5Stack's driver, github.com/m5stack/M5Unit-KEYBOARD (unit_Tab5Keyboard.hpp, MIT).
 *  2. Any ordinary USB keyboard in the USB-A port (drivers/usbkbd).
 *
 * Either way what is typed goes to the console input (kconsole_inject), like serial input.
 * HID mode: the queue holds (modifier, usage) pairs.  A press has a non-zero usage; the release is
 * reported as an all-zero pair.  The accessory's firmware already adds Shift for keys whose printed
 * symbol is the shifted one (e.g. '!'), so the standard US-layout HID decoding gives the right character.
 */
#include "../keyboard/keyboard.h"
#include "../usbkbd/usbkbd.h"
#include "../usbkbd/hid_keys.h"
#include "osk.h"
#include <m5os/ime.h>
#include <m5os/keyboard_raw.h>
#include <m5os/hal_i2c.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/hal_time.h>

#define KBD_ADDR        0x6D
#define PIN_SDA         0            /* Ext.Port1 */
#define PIN_SCL         1
#define REG_EVENT_NUM   0x02         /* queue length 0..32, decrements as events are read */
#define REG_MODE        0x10         /* 0 = normal (row/col), 1 = HID, 2 = character */
#define REG_HID_EVENT   0x30         /* modifier at 0x30, usage at 0x31 */
#define REG_FIRMWARE    0xFE
#define MODE_HID        1
#define EVENT_EMPTY     0xFF         /* both bytes 0xFF: nothing queued */
#define POLL_MS         10u
#define MAX_PER_POLL    8            /* events handled per poll, so a flood cannot starve other tasks */
#define REPEAT_DELAY_MS 400u
#define REPEAT_PERIOD_MS 60u
#define TASK_STACK      8192         /* the input method converts on this stack */

static const char *s_type = "none";
static keyboard_stats_t s_stats = { 0, -1, -1, -1, 0 };
static bool s_caps;

static void emit(uint8_t usage, uint8_t mods)
{
    if(keyboard_raw_active())return;
    char buf[8];
    hid_decode(usage, mods, s_caps, buf);
    if (buf[0]) { ime_feed(buf); s_stats.last_char = (unsigned char)buf[0]; }
}

static void kbd_task(void *arg)
{
    (void)arg;
    uint8_t held = 0, held_mods = 0;
    bool native=false;
    uint32_t epoch=0;
    uint64_t pressed_at = 0, last_repeat = 0;

    for (;;) {
        task_sleep(POLL_MS);
        bool wanted=keyboard_raw_active();
        uint32_t gen=keyboard_raw_generation();
        if(wanted!=native||(wanted&&gen!=epoch)){
            uint8_t mode=wanted?0:MODE_HID,actual=0xff;
            if(!hal_i2c_write_reg(KBD_ADDR,REG_MODE,mode) ||
               !hal_i2c_read_reg(KBD_ADDR,REG_MODE,&actual) || actual!=mode ||
               !hal_i2c_write_reg(KBD_ADDR,REG_EVENT_NUM,0)){
                keyboard_raw_clear();s_stats.i2c_errors++;continue;
            }
            native=wanted;epoch=gen;held=0;keyboard_raw_clear();
            kputs(native?"keyboard: game input active\n":"keyboard: text input restored\n");
        }
        uint8_t count = 0;
        if (!hal_i2c_read_reg(KBD_ADDR, REG_EVENT_NUM, &count)) { keyboard_raw_clear();s_stats.i2c_errors++; continue; }
        if(native){
            if(count>=32){keyboard_raw_clear();hal_i2c_write_reg(KBD_ADDR,REG_EVENT_NUM,0);continue;}
            keyboard_raw_ready(epoch);
            for(unsigned n=0;n<MAX_PER_POLL && count;n++,count--){
                uint8_t event=0xff;
                if(!hal_i2c_read_reg(KBD_ADDR,0x20,&event)){keyboard_raw_clear();s_stats.i2c_errors++;break;}
                if(event==0xff)break;
                if(event&0x80){s_stats.presses++;s_stats.last_x=event&15;s_stats.last_y=(event>>4)&7;}
                keyboard_raw_matrix(epoch,event);
            }
            continue;
        }
        for (int n = 0; n < MAX_PER_POLL && count > 0; n++, count--) {
            uint8_t ev[2];
            if (!hal_i2c_read_regs(KBD_ADDR, REG_HID_EVENT, ev, 2)) { s_stats.i2c_errors++; break; }
            if (ev[0] == EVENT_EMPTY && ev[1] == EVENT_EMPTY) break;
            uint64_t now = hal_time_now_us();
            if (ev[1] == 0) { held = 0; continue; }                        /* release */
            if (ev[1] == HID_USAGE_CAPSLOCK) { s_caps = !s_caps; continue; }
            s_stats.presses++; s_stats.last_x = ev[1];
            emit(ev[1], ev[0]);
            held = ev[1]; held_mods = ev[0]; pressed_at = last_repeat = now;
        }
        uint64_t now = hal_time_now_us();
        if (held && now - pressed_at > REPEAT_DELAY_MS * 1000u && now - last_repeat > REPEAT_PERIOD_MS * 1000u) {
            emit(held, held_mods);                                         /* auto-repeat while held */
            last_repeat = now;
        }
    }
}

const char *keyboard_type(void) { return s_type[0] == 'n' ? usbkbd_type() : s_type; }

void keyboard_stats(keyboard_stats_t *out)
{
    if (s_type[0] == 'n') usbkbd_stats(out); else *out = s_stats;
}

void keyboard_start(void)
{
    ime_init();                                       /* Japanese input: keys pass through it (off until Ctrl+Space) */
    hal_i2c_set_pins(PIN_SCL, PIN_SDA);              /* legacy helpers use Ext.Port1; fixed-system helpers still serve touch/camera/power */
    hal_i2c_init();
    uint8_t fw = 0;
    if (hal_i2c_read_reg(KBD_ADDR, REG_FIRMWARE, &fw)) {
        hal_i2c_write_reg(KBD_ADDR, REG_MODE, MODE_HID);
        hal_i2c_write_reg(KBD_ADDR, REG_EVENT_NUM, 0);                   /* drop anything queued before we started */
        s_type = "tab5-i2c";
        kprintf("keyboard: Tab5 Keyboard accessory on Ext.Port1 (firmware %02x)\n", fw);
        task_create_stack("kbd", kbd_task, NULL, TASK_PRIO_DEFAULT + 2, TASK_STACK);
        osk_start();                                  /* the on-screen keyboard is there too (hide it with `osk off`) */
        return;
    }
    kprintf("keyboard: no Tab5 Keyboard accessory (I2C 0x%02x), looking for a USB keyboard\n", KBD_ADDR);
    usbkbd_start();
    osk_start();                                      /* touch screen keyboard, so the shell works with no keyboard at all */
}
