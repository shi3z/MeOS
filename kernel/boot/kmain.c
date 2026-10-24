#include <m5os/kernel.h>
#include <m5os/crash.h>
#include <m5os/health.h>
#include <m5os/kprintf.h>
#include <m5os/hal_console.h>
#include <m5os/hal_time.h>
#include <m5os/task.h>
#include <m5os/arch.h>
#include <m5os/display.h>
#include <m5os/kmalloc.h>
#include <m5os/device.h>
#include <m5os/errno.h>
#include <m5os/dev_display.h>
#include <m5os/dev_buttons.h>
#include <m5os/blockdev.h>
#include <m5os/fs.h>
#include <m5os/vfs.h>
#include <m5os/mutex.h>
#include <m5os/app.h>
#include <m5os/m5ex.h>
#include <m5os/msg.h>
#include <m5os/gui.h>
#include <m5os/wifi.h>
#include <m5os/board_config.h>
#if M5OS_HAS_SD
#include "../../drivers/sd/sd_spi.h"
#endif
#include "../../user/shell/shell.h"
#if M5OS_HAS_KEYBOARD
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/lcd/fbcon.h"
#endif
#include "../../apps/apps.h"

void hog_main(void *arg);

/* Build with M5OS_SELFTEST=1 in the environment to run the regression tests at
 * boot (instead of starting the shell). */
#ifndef M5OS_BOOT_SELFTEST
#define M5OS_BOOT_SELFTEST 0
#endif

bool rootfs_mount(blockdev_t *sd);
static blockdev_t *s_sd;
static bool lcd_ready;


#if M5OS_BOOT_SELFTEST
/* =========================================================================
 * Status screen task.  Uses only the VFS: /dev/display and /dev/buttons.
 * ========================================================================= */

#define UI_PERIOD_MS    100u
#define VALUE_X         8
#define VALUE_Y0        44
#define VALUE_LINE_H    12
#define TERM_Y0         120
#define TERM_Y1         196
#define BTN_Y           (DISPLAY_HEIGHT - BUTTONS_STRIP_H)
#define BTN_W           (DISPLAY_WIDTH / 3)

static int put_u32(char *dst, uint32_t v)
{
    char tmp[11]; int n = 0, k = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) dst[k++] = tmp[--n];
    dst[k] = 0;
    return k;
}

static void dtext(int disp, int x, int y, uint16_t fg, uint16_t bg, int scale, const char *s)
{
    display_text_t t = { .x = (int16_t)x, .y = (int16_t)y, .fg = fg, .bg = bg, .scale = (uint8_t)scale, .text = s };
    vfs_ioctl(disp, DISPLAY_IOC_DRAW_TEXT, &t);
}

static void value_line(int disp, int row, const char *label, uint32_t v, uint16_t color)
{
    char buf[32]; int k = 0;
    for (const char *p = label; *p && k < 18; p++) buf[k++] = *p;
    k += put_u32(buf + k, v);
    while (k < 30) buf[k++] = ' ';                   /* pad: overwrites stale digits */
    buf[k] = 0;
    dtext(disp, VALUE_X, VALUE_Y0 + row * VALUE_LINE_H, color, COLOR_BLACK, 1, buf);
}

static void draw_button(int disp, int idx, bool pressed)
{
    static const char names[3][2] = { "A", "B", "C" };
    display_rect_t r = { .x = (int16_t)(idx * BTN_W + 2), .y = BTN_Y + 2, .w = BTN_W - 4,
                         .h = BUTTONS_STRIP_H - 4, .color = pressed ? COLOR_GREEN : COLOR_GRAY };
    vfs_ioctl(disp, DISPLAY_IOC_FILL_RECT, &r);
    dtext(disp, idx * BTN_W + BTN_W / 2 - 8, BTN_Y + 12, COLOR_BLACK, r.color, 2, names[idx]);
}

static void ui_task(void *arg)
{
    (void)arg;
    int disp = vfs_open("/dev/display", VFS_O_READ | VFS_O_WRITE);
    int btn  = vfs_open("/dev/buttons", VFS_O_READ);
    if (disp < 0) { kprintf("ui: cannot open /dev/display (%d)\n", disp); return; }

    uint16_t black = COLOR_BLACK;
    vfs_ioctl(disp, DISPLAY_IOC_CLEAR, &black);
    dtext(disp, 8, 4, COLOR_WHITE, COLOR_BLACK, 3, "MeOS 0.1");
    dtext(disp, 8, 30, COLOR_RED, COLOR_BLACK, 1, "FreeRTOS: disabled");
    display_term_t term = { .y0 = TERM_Y0, .y1 = TERM_Y1, .fg = COLOR_CYAN, .bg = COLOR_BLACK, .scale = 1 };
    vfs_ioctl(disp, DISPLAY_IOC_SET_TERM, &term);
    for (int i = 0; i < 3; i++) draw_button(disp, i, false);

    uint8_t shown = 0;
    for (;;) {
        task_info_t ti[TASK_MAX];
        value_line(disp, 0, "uptime ms ", (uint32_t)(hal_time_now_us() / 1000u), COLOR_WHITE);
        value_line(disp, 1, "tasks ",     (uint32_t)task_snapshot(ti, TASK_MAX), COLOR_GREEN);
        value_line(disp, 2, "hog count ", app_hog_count, COLOR_GREEN);
        value_line(disp, 3, "ticks ",     sched_ticks(),  COLOR_YELLOW);
        kmem_stats_t mi, mp;
        kmem_internal_stats(&mi);
        kmem_psram_stats(&mp);
        value_line(disp, 4, "sram free KB ",  (uint32_t)(mi.free / 1024u), COLOR_WHITE);
        value_line(disp, 5, "psram free KB ", (uint32_t)(mp.free / 1024u), COLOR_WHITE);

        if (btn >= 0) {
            uint8_t mask = 0;
            vfs_read(btn, &mask, 1);
            if (mask != shown) {
                for (int i = 0; i < 3; i++) draw_button(disp, i, (mask >> i) & 1u);
                shown = mask;
            }
            buttons_touch_t t;
            vfs_ioctl(btn, BUTTONS_IOC_GET_TOUCH, &t);
            char buf[32]; int k = 0;
            const char *lab = t.pressed ? "touch x=" : "touch --       ";
            for (const char *p = lab; *p; p++) buf[k++] = *p;
            if (t.pressed) {
                k += put_u32(buf + k, t.x);
                buf[k++] = ' '; buf[k++] = 'y'; buf[k++] = '=';
                k += put_u32(buf + k, t.y);
            }
            while (k < 30) buf[k++] = ' ';
            buf[k] = 0;
            dtext(disp, VALUE_X, VALUE_Y0 + 6 * VALUE_LINE_H, COLOR_YELLOW, COLOR_BLACK, 1, buf);
        }
        task_sleep(UI_PERIOD_MS);
    }
}

#endif

#if M5OS_BOOT_SELFTEST
/* =========================================================================
 * Regression tests (developer builds only).
 * ========================================================================= */
#define LOG_LINES  100
#define LOG_PAD    18

/* Two writers that print whole lines to the console AND the display terminal at
 * once, while hogs saturate the CPU: proves the console / device mutexes. */
static void log_task(void *arg)
{
    char id = (char)(uintptr_t)arg;
    int disp = vfs_open("/dev/display", VFS_O_WRITE);
    int uart = vfs_open("/dev/uart", VFS_O_WRITE);
    uint32_t seed = (uint32_t)id * 2654435761u;
    for (uint32_t n = 0; n < LOG_LINES; n++) {
        kprintf("[%c] line %03u ................\n", id, n);
        if (disp >= 0) {
            char line[LOG_PAD + 1] = { id, ' ' };
            put_u32(line + 2, n);
            int k = 2; while (line[k]) k++;
            line[k++] = '\n'; line[k] = 0;
            vfs_write(disp, line, (size_t)k);
        }
        if (uart >= 0 && n == LOG_LINES - 1) {
            static const char done[] = "uart device write ok\n";
            vfs_write(uart, done, sizeof done - 1);
        }
        seed = seed * 1664525u + 1013904223u;
        task_sleep(seed >> 29);
    }
    kprintf("[%c] log done\n", id);
}

#define CHURN_TASKS          20
#define CHURN_MAX_LATENCY_MS 150u
static volatile uint32_t s_churn_max_latency;

static void tmp_task(void *arg)
{
    uint32_t lat = (uint32_t)(hal_time_now_us() / 1000u) - (uint32_t)(uintptr_t)arg;
    if (lat > s_churn_max_latency) s_churn_max_latency = lat;
}

/* task_kill must release mutexes the victim owned, or one kill can freeze the system. */
static mutex_t s_kill_m;
static volatile int s_waiter_got;

static void holder_task(void *arg)
{
    (void)arg;
    mutex_lock(&s_kill_m);
    for (;;) task_sleep(1000);                       /* never unlocks */
}

static void waiter_task(void *arg)
{
    (void)arg;
    mutex_lock(&s_kill_m);                           /* blocks behind the holder */
    s_waiter_got = 1;
    mutex_unlock(&s_kill_m);
}

static bool kill_selftest(void)
{
    kmem_stats_t before, after;
    kmem_internal_stats(&before);
    mutex_init(&s_kill_m);
    s_waiter_got = 0;
    int h = task_create("holder", holder_task, NULL, TASK_PRIO_DEFAULT + 2);
    task_sleep(50);
    int w = task_create("waiter", waiter_task, NULL, TASK_PRIO_DEFAULT + 2);
    task_sleep(50);
    bool ok = h > 0 && w > 0 && !s_waiter_got;       /* the waiter really is stuck */
    ok &= task_kill((uint32_t)h) == ERR_OK;
    task_sleep(100);
    ok &= s_waiter_got == 1;                         /* ... and the kill freed the mutex */
    ok &= task_kill(1) == ERR_INVAL;                 /* idle */
    ok &= task_kill(task_current()->id) == ERR_INVAL;/* self */
    ok &= task_kill(9999) == ERR_NOENT;
    ok &= task_kill((uint32_t)h) == ERR_NOENT;       /* already dead */
    task_sleep(50);
    kmem_internal_stats(&after);
    ok &= after.used == before.used;                 /* killed task's stack was reclaimed */
    kprintf("kill selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ---- application lifecycle ------------------------------------------------ */
static bool app_info_of(uint32_t id, app_info_t *out)
{
    app_info_t all[APP_MAX];
    int n = app_list(all, APP_MAX);
    for (int i = 0; i < n; i++) if (all[i].app_id == id) { *out = all[i]; return true; }
    return false;
}

static int live_tasks(void) { task_info_t t[TASK_MAX]; return task_snapshot(t, TASK_MAX); }

#define SETTLE_MS  250u      /* appd runs every 20 ms; allow several periods */

#define CK(c) do { if (!(c)) { kprintf("  FAIL line %d: %s\n", __LINE__, #c); ok = false; } } while (0)

static bool app_selftest(void)
{
    bool ok = true;
    uint32_t id;
    app_info_t info;
    kmem_stats_t base, now;
    int tasks0 = live_tasks();
    kmem_internal_stats(&base);

    /* unknown names */
    CK(app_start("no-such-app", &id) == ERR_NOENT);
    CK(app_stop(9999) == ERR_NOENT);

    /* normal exit: RUNNING -> EXITED */
    CK(app_start("hello", &id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(app_info_of(id, &info) && info.state == APP_EXITED && info.tasks == 0);
    CK(app_stop(id) == ERR_NOENT);                            /* already finished */

    /* permissions: rogue holds only CONSOLE */
    app_rogue_mask = 0;
    CK(app_start("rogue", &id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(app_rogue_mask == 0x3Fu);                               /* display, file write, spawn, ipc and window all DENIED; console allowed */
    CK(vfs_stat("/home/rogue.txt", &(vfs_info_t){ 0 }) == ERR_NOENT);      /* ... and the file really was not created */

    /* a well-behaved app with file permissions */
    CK(app_start("notes", &id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(vfs_stat("/home/notes.txt", &(vfs_info_t){ 0 }) == ERR_OK);
    vfs_unlink("/home/notes.txt");

    /* resource reclamation: memory + handles of a stopped app */
    app_leaker_ready = 0;
    CK(app_start("leaker", &id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(app_leaker_ready == 1);
    CK(app_info_of(id, &info) && info.state == APP_RUNNING && info.mem_bytes >= 28672u);
    CK(vfs_count_owned(id) == 3);
    CK(app_stop(id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(app_info_of(id, &info) && info.state == APP_KILLED && info.mem_bytes == 0 && info.reclaimed_bytes >= 28672u);
    CK(vfs_count_owned(id) == 0);

    /* a family of tasks: stopping the app ends the child too */
    app_family_child_pid = 0;
    CK(app_start("family", &id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(app_family_child_pid != 0 && app_info_of(id, &info) && info.tasks == 2);
    CK(app_stop(id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(app_info_of(id, &info) && info.state == APP_KILLED && info.tasks == 0);

    /* killing the owner task directly (shell "kill") is the same as stopping the app */
    CK(app_start("hog", &id) == ERR_OK);
    task_sleep(50);
    CK(app_info_of(id, &info) && task_kill(info.owner_task) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(app_info_of(id, &info) && info.state == APP_KILLED);

    /* nothing leaked: same heap use, same number of tasks as before */
    task_sleep(SETTLE_MS);
    kmem_internal_stats(&now);
    CK(now.used == base.used);
    CK(live_tasks() == tasks0);
    kprintf("app selftest: %s%s\n", ok ? "PASS" : "FAIL",
            (now.used == base.used && live_tasks() == tasks0) ? "" : "  (heap or task leak)");
    return ok;
}

static void selftest_task(void *arg)
{
    (void)arg;
    bool ok = kmem_selftest();
    kmem_stats_t before, after;
    kmem_internal_stats(&before);
    for (int i = 0; i < CHURN_TASKS; i++) {
        uint32_t now_ms = (uint32_t)(hal_time_now_us() / 1000u);
        if (task_create("tmp", tmp_task, (void *)(uintptr_t)now_ms, TASK_PRIO_DEFAULT) < 0) {
            kprintf("churn: task_create failed at iteration %d (task table full?)\n", i);
            ok = false;
            break;
        }
        task_sleep(30);
    }
    task_sleep(100);
    kmem_internal_stats(&after);
    bool leak_free = after.used == before.used;
    bool fair = s_churn_max_latency <= CHURN_MAX_LATENCY_MS;
    kprintf("kmem selftest: task churn stack leak %s\n", leak_free ? "NONE" : "DETECTED");
    kprintf("sched selftest: equal-priority start latency max %u ms (limit %u)  %s\n",
            s_churn_max_latency, CHURN_MAX_LATENCY_MS, fair ? "PASS" : "FAIL");
    kprintf("MEMTEST %s\n", (ok && leak_free && fair) ? "ALL PASS" : "FAILED");
    kprintf("KILLTEST %s\n", kill_selftest() ? "ALL PASS" : "FAILED");

    /* Strictly AFTER the memory tests (they measure heap totals and start latency). */
    bool fs_ok = fs_selftest();
    kprintf("FSTEST %s\n", fs_ok ? "ALL PASS" : "FAILED");

    /* The M5FS test needs the single volume to itself; only now mount the real root. */
    bool root_ok = rootfs_mount(s_sd);
    kprintf("mounts:\n");
    vfs_print_mounts();
    bool vfs_ok = root_ok && vfs_selftest();
    kprintf("VFSTEST %s\n", vfs_ok ? "ALL PASS" : "FAILED");

    bool app_ok = root_ok && app_selftest();
    kprintf("APPTEST %s\n", app_ok ? "ALL PASS" : "FAILED");

    bool m5_ok = root_ok && m5ex_selftest();
    kprintf("M5EXTEST %s\n", m5_ok ? "ALL PASS" : "FAILED");

    bool msg_ok = root_ok && msg_selftest();
    kprintf("MSGTEST %s\n", msg_ok ? "ALL PASS" : "FAILED");

#if M5OS_HAS_TOUCH
    bool gui_ok = root_ok && lcd_ready && gui_selftest();
    kprintf("GUITEST %s\n", gui_ok ? "ALL PASS" : "FAILED");
#endif
}
#endif

void kernel_main(void)
{
    hal_console_init();
    hal_time_init();
    crash_boot_report();
    kprintf("\nMeOS %s booting...\n", M5OS_VERSION_STRING);
    kprintf("Board: %s\n", M5OS_BOARD_NAME);
    kprintf("CPU: %s\nScheduler: MeOS (preemptive, %u ms tick)\nFreeRTOS: disabled\n",
            M5OS_CPU_NAME, SCHED_TICK_US / 1000u);

    kmem_init();
    kmem_stats_t mi, mp;
    kmem_internal_stats(&mi);
    kmem_psram_stats(&mp);
    kprintf("Heap: internal %u KB, PSRAM %u KB\n", (uint32_t)(mi.total / 1024u), (uint32_t)(mp.total / 1024u));

    bool lcd_ok = display_init();                    /* driver bring-up (kernel side) */
    kprintf("LCD: %s\n", lcd_ok ? "ok" : "FAILED");
    lcd_ready = lcd_ok;

    sched_init();
    devices_init();                                  /* registers uart/display/buttons */
    vfs_init();                                      /* /dev is available from here on */
    app_init();                                      /* cleanup task for finished applications */

    /* SD card: read-only probe. A card is never written or formatted automatically. */
#if M5OS_HAS_SD
    blockdev_t *sd = s_sd = sd_init();
    if (sd) sd_print_info();
#else
    blockdev_t *sd = s_sd = NULL;                    /* no SD driver on this board yet */
#endif

#if M5OS_BOOT_SELFTEST
    task_create("hog0", hog_main, NULL, TASK_PRIO_DEFAULT);
    task_create("hog1", hog_main, NULL, TASK_PRIO_DEFAULT);
    if (lcd_ok) task_create("ui", ui_task, NULL, TASK_PRIO_DEFAULT + 2);
    task_create_stack("selftest", selftest_task, NULL, TASK_PRIO_DEFAULT + 1, 8192);
    task_create("logA", log_task, (void *)'A', TASK_PRIO_DEFAULT);
    task_create("logB", log_task, (void *)'B', TASK_PRIO_DEFAULT);
#else
    rootfs_mount(sd);
    wifi_boot_start();
#if M5OS_HAS_TOUCH
    if (lcd_ok && gui_init() == ERR_OK) {            /* touch screen: windows and the launcher */
        uint32_t id;
        app_start("launcher", &id);                  /* the home screen */
    }
#endif
#if M5OS_HAS_KEYBOARD
    if (lcd_ok) fbcon_start();                       /* no touch: the whole screen is the console */
    keyboard_start();                                /* typed characters go to the shell like serial input */
#endif
    task_create_stack("shell", shell_task, NULL, TASK_PRIO_DEFAULT + 1, 32768);
#endif
    health_start();
    sched_start_preemption();

    for (;;) arch_cpu_idle();
}
