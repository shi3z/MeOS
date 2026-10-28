/*
 * USB keyboard for the Tab5 (USB-A host port).  What is typed goes to the console input (kconsole_inject),
 * so the shell reads it like characters arriving over the serial port.  Started by drivers/tab5kbd when
 * there is no I2C keyboard.
 *
 * How a USB keyboard is read here:
 *  1. Wait for a device, give the bus time to settle, reset the port (all USB host rules, USB 2.0 ch. 7/9).
 *  2. Enumerate with control transfers on endpoint 0: read the device descriptor, give the device
 *     address 1, read the configuration descriptor, select configuration 1.
 *  3. Find the HID "boot keyboard" interface (class 3, subclass 1, protocol 1) and switch it to the boot
 *     protocol: an 8-byte report every keyboard understands (USB HID 1.11, appendix B) -
 *       byte 0 modifier bits | byte 1 reserved | bytes 2..7 up to six key codes currently held.
 *  4. Poll with GET_REPORT on endpoint 0 about every 10 ms.  A real stack would use the interrupt IN
 *     endpoint the keyboard offers; that needs the controller's periodic schedule, and control polling is
 *     enough for typing.  (Keyboards are required to answer GET_REPORT for the boot protocol.)
 * Key codes are HID usage IDs, US layout.
 */
#include "../keyboard/keyboard.h"
#include "usbkbd.h"
#include "hid_keys.h"
#include <m5os/ime.h>
#include <m5os/hal_usbh.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/hal_time.h>
#include <string.h>

#define TASK_STACK        8192       /* the input method converts on this stack */
#define POLL_MS           10u
#define CONNECT_POLL_MS   50u
#define DEBOUNCE_MS       250u        /* USB 2.0 7.1.7.3: wait for the connection to be stable */
#define RESET_HOLD_MS     30u         /* IDF's default; the spec asks >= 10 ms for a root port */
#define RESET_RECOVERY_MS 30u
#define ENABLE_WAIT_MS    200u
#define ADDR_RECOVERY_MS  10u         /* SET_ADDRESS: device may take up to 2 ms to respond at the new address */
#define KBD_ADDRESS       1
#define EP0_MPS_LOW       8u
#define EP0_MPS_FULL      64u
#define MAX_FAILS         20          /* consecutive failed polls before we assume the keyboard is gone */
#define REPEAT_DELAY_MS   400u
#define REPEAT_PERIOD_MS  50u

/* Standard requests (USB 2.0 table 9-4) and HID class requests (HID 1.11 section 7.2) */
#define REQ_GET_DESCRIPTOR   0x06
#define REQ_SET_ADDRESS      0x05
#define REQ_SET_CONFIG       0x09
#define HID_GET_REPORT       0x01
#define HID_SET_IDLE         0x0A
#define HID_SET_PROTOCOL     0x0B
#define DESC_DEVICE          1
#define DESC_CONFIG          2
#define DESC_INTERFACE       4
#define HID_CLASS            3
#define HID_SUBCLASS_BOOT    1
#define HID_PROTOCOL_KBD     1
#define HID_REPORT_INPUT     1
#define REPORT_BYTES         8


static const char *s_type = "none";
static keyboard_stats_t s_stats = { 0, -1, -1, -1, 0 };
static bool s_caps;

const char *usbkbd_type(void) { return s_type; }
void usbkbd_stats(keyboard_stats_t *out) { *out = s_stats; }

static void setup_packet(uint8_t s[8], uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
    s[0] = type; s[1] = req;
    s[2] = (uint8_t)value; s[3] = (uint8_t)(value >> 8);
    s[4] = (uint8_t)index; s[5] = (uint8_t)(index >> 8);
    s[6] = (uint8_t)len;   s[7] = (uint8_t)(len >> 8);
}

static int ctrl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len)
{
    uint8_t s[8];
    setup_packet(s, type, req, value, index, len);
    return hal_usbh_control(s, data, len);
}

/* ---- key decoding --------------------------------------------------------------- */

static void emit(uint8_t usage, uint8_t mods)
{
    char buf[8];
    hid_decode(usage, mods, s_caps, buf);
    if (buf[0]) { ime_feed(buf); s_stats.last_char = (unsigned char)buf[0]; }
}

/* ---- enumeration ---------------------------------------------------------------- */

static bool wait_enabled(void)
{
    for (uint32_t t = 0; t < ENABLE_WAIT_MS; t += 5) {
        if (hal_usbh_poll() == USBH_EVENT_ENABLED) return true;
        task_sleep(5);
    }
    return false;
}

/* Returns the HID boot keyboard's interface number, or -1. Sets *config to the configuration value. */
static int enumerate(usbh_speed_t speed, int *config)
{
    uint8_t d[64];
    uint16_t mps0 = speed == USBH_SPEED_LOW ? EP0_MPS_LOW : EP0_MPS_FULL;
    if (!hal_usbh_open_control(0, mps0)) { kprintf("usbkbd: no control channel\n"); return -1; }

    int n = ctrl(0x80, REQ_GET_DESCRIPTOR, DESC_DEVICE << 8, 0, d, 18);
    if (n < 8) { kprintf("usbkbd: device descriptor read failed (%d)\n", n); return -1; }
    uint16_t vid = (uint16_t)(d[8] | d[9] << 8), pid = (uint16_t)(d[10] | d[11] << 8);
    mps0 = d[7];
    kprintf("usbkbd: device %04x:%04x, class %u, ep0 packet %u, %s speed\n", vid, pid, d[4], mps0,
            speed == USBH_SPEED_LOW ? "low" : speed == USBH_SPEED_FULL ? "full" : "high");

    if (ctrl(0x00, REQ_SET_ADDRESS, KBD_ADDRESS, 0, NULL, 0) < 0) { kprintf("usbkbd: SET_ADDRESS failed\n"); return -1; }
    task_sleep(ADDR_RECOVERY_MS);
    if (!hal_usbh_open_control(KBD_ADDRESS, mps0)) return -1;

    uint8_t cfg[255];
    n = ctrl(0x80, REQ_GET_DESCRIPTOR, DESC_CONFIG << 8, 0, cfg, 9);
    if (n < 9) { kprintf("usbkbd: config descriptor read failed (%d)\n", n); return -1; }
    uint16_t total = (uint16_t)(cfg[2] | cfg[3] << 8);
    if (total > sizeof cfg) total = sizeof cfg;
    n = ctrl(0x80, REQ_GET_DESCRIPTOR, DESC_CONFIG << 8, 0, cfg, total);
    if (n < 9) { kprintf("usbkbd: full config descriptor read failed (%d)\n", n); return -1; }
    *config = cfg[5];

    for (int i = 0; i + 9 <= n && cfg[i] >= 2; i += cfg[i]) {                    /* walk the descriptors */
        if (cfg[i + 1] == DESC_INTERFACE && cfg[i + 5] == HID_CLASS && cfg[i + 6] == HID_SUBCLASS_BOOT && cfg[i + 7] == HID_PROTOCOL_KBD)
            return cfg[i + 2];
    }
    kprintf("usbkbd: device has no HID boot keyboard interface\n");
    return -1;
}

/* ---- the task ------------------------------------------------------------------- */

static void keyboard_session(int iface)
{
    uint8_t prev[REPORT_BYTES] = { 0 }, cur[REPORT_BYTES];
    uint8_t held = 0;                                   /* the key that auto-repeats */
    uint64_t pressed_at = 0, last_repeat = 0;
    int fails = 0;

    for (;;) {
        task_sleep(POLL_MS);
        usbh_event_t ev = hal_usbh_poll();
        if (ev == USBH_EVENT_DISCONNECT || ev == USBH_EVENT_DISABLED) return;

        int n = ctrl(0xA1, HID_GET_REPORT, (HID_REPORT_INPUT << 8), (uint16_t)iface, cur, REPORT_BYTES);
        if (n < REPORT_BYTES) {
            s_stats.i2c_errors++;                       /* (field reused as "transfer errors") */
            if (++fails >= MAX_FAILS) return;
            continue;
        }
        fails = 0;

        uint8_t mods = cur[0];
        uint64_t now = hal_time_now_us();
        for (int i = 2; i < REPORT_BYTES; i++) {
            uint8_t k = cur[i];
            if (k < HID_USAGE_FIRST) continue;              /* 0 = none, 1..3 = rollover / error codes */
            bool was_down = false;
            for (int j = 2; j < REPORT_BYTES; j++) if (prev[j] == k) was_down = true;
            if (was_down) continue;
            s_stats.presses++; s_stats.last_x = k;
            if (k == HID_USAGE_CAPSLOCK) { s_caps = !s_caps; continue; }
            emit(k, mods);
            held = k; pressed_at = last_repeat = now;
        }
        bool held_still = false;
        for (int i = 2; i < REPORT_BYTES; i++) if (cur[i] == held && held) held_still = true;
        if (!held_still) held = 0;
        else if (now - pressed_at > REPEAT_DELAY_MS * 1000u && now - last_repeat > REPEAT_PERIOD_MS * 1000u) {
            emit(held, mods);
            last_repeat = now;
        }
        memcpy(prev, cur, sizeof prev);
    }
}

static void usbkbd_task(void *arg)
{
    (void)arg;
    if (!hal_usbh_init()) { kprintf("usbkbd: USB host init failed\n"); return; }
    kprintf("usbkbd: waiting for a USB keyboard on the USB-A port\n");

    for (;;) {
        s_type = "none";
        while (hal_usbh_poll() != USBH_EVENT_CONNECT) task_sleep(CONNECT_POLL_MS);

        task_sleep(DEBOUNCE_MS);
        hal_usbh_debounce_done();
        if (!hal_usbh_connected()) continue;

        hal_usbh_reset(true);
        task_sleep(RESET_HOLD_MS);
        hal_usbh_reset(false);
        if (!wait_enabled()) { kprintf("usbkbd: port did not come up after reset\n"); continue; }
        task_sleep(RESET_RECOVERY_MS);

        int config = 0;
        int iface = enumerate(hal_usbh_speed(), &config);
        if (iface >= 0 && ctrl(0x00, REQ_SET_CONFIG, (uint16_t)config, 0, NULL, 0) >= 0) {
            ctrl(0x21, HID_SET_PROTOCOL, 0, (uint16_t)iface, NULL, 0);           /* 0 = boot protocol */
            ctrl(0x21, HID_SET_IDLE, 0, (uint16_t)iface, NULL, 0);               /* may STALL: not required */
            s_type = "usb-hid";
            kprintf("usbkbd: keyboard ready (interface %d)\n", iface);
            keyboard_session(iface);
            kprintf("usbkbd: keyboard gone\n");
        }
        /* wait for the cable to be unplugged (or the port to drop) before trying again */
        while (hal_usbh_connected()) { hal_usbh_poll(); task_sleep(CONNECT_POLL_MS); }
    }
}

void usbkbd_start(void)
{
    task_create_stack("usbkbd", usbkbd_task, NULL, TASK_PRIO_DEFAULT + 2, TASK_STACK);
}
