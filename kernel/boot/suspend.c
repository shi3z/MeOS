/*
 * The power daemon: watches the idle timer, puts the Tab5 into suspend and wakes it.  The decisions are in
 * kernel/lib/suspend_core.c (tested on the host); this file does the hardware.
 *
 * Suspend, in this order (the user sees the screen go dark first):
 *   backlight off -> panel off -> drawing frozen -> extension port 5 V off -> CPU clock down.
 * Wake, the reverse (the CPU first, so the rest is quick):
 *   CPU clock up -> 5 V back -> panel on -> drawing allowed -> backlight back to what it was.
 *
 * "Frozen" means display_set_suspended(): every drawing call is discarded, so the framebuffer keeps the last
 * picture and programs that keep drawing waste no time on it.  The console (fbcon) holds back its changes and
 * paints them on wake; full-screen programs are asked to repaint (DQK_REFRESH); WebAssembly apps are held in
 * their frame loop (suspend_wait_if_active).
 *
 * What stays untouched on purpose: the USB-A port's 5 V (the keyboard keeps its power), Wi-Fi, Tailscale and
 * the SSH sessions (their tasks keep running, just at the lower clock).
 */
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/suspend.h>
#include <m5os/suspend_core.h>
#include <m5os/display.h>
#include <m5os/board.h>
#include <m5os/hal_cpu.h>
#include <m5os/hal_time.h>
#include <m5os/imu.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/arch.h>
#include <string.h>

#define AWAKE_POLL_MS       200u        /* how often the idle timer is looked at */
#define SUSPENDED_POLL_MS   50u         /* 20 Hz: wake-up inputs and the accelerometer */
#define SUSPEND_CPU_MHZ     90u         /* a quarter of the 360 MHz clock; the PLL keeps running */
#define MOTION_THRESHOLD    1200        /* sum of the three axis changes between two samples; 4096 counts = 1 g, so about 0.3 g */
#define IMU_START_DELAY_MS  15000u      /* the accelerometer is set up in the background, after boot has settled */
#define IDLE_MIN_SECONDS    10u
#define IDLE_MAX_SECONDS    86400u
#define FULL_BRIGHTNESS     255u
#define TASK_STACK          6144

static suspend_core_t s_core;
static motion_detector_t s_motion;
static uint8_t  s_saved_backlight;
static bool     s_ext5v_was_on;
static unsigned s_saved_mhz;
static uint32_t s_last_resume_ms;
static bool     s_started;

static uint64_t now_us(void) { return hal_time_now_us(); }

bool suspend_active(void) { return s_core.suspended; }

bool suspend_filter_input(void)
{
    if (!s_started) return false;
    uint32_t irq = arch_irq_disable();
    bool consumed = suspend_core_input(&s_core, now_us());
    arch_irq_restore(irq);
    return consumed;
}

void suspend_request(void)
{
    uint32_t irq = arch_irq_disable();
    suspend_core_request_enter(&s_core);
    arch_irq_restore(irq);
}

uint32_t suspend_poll_ms(uint32_t normal_ms)
{
    return (s_core.suspended && normal_ms < SUSPENDED_POLL_MS) ? SUSPENDED_POLL_MS : normal_ms;
}

void suspend_wait_if_active(void)
{
    while (s_core.suspended) task_sleep(SUSPENDED_POLL_MS);
}

static void serial_activity(void)               /* typing on the serial console counts, but is not swallowed */
{
    uint32_t irq = arch_irq_disable();
    suspend_core_wake(&s_core, now_us());
    arch_irq_restore(irq);
}

static void do_enter(void)
{
    kprintf("suspend: entering; any key, touch or movement wakes\n");
    s_saved_backlight = board_backlight_get();
    board_backlight(0);
    display_panel_power(false);
    display_set_suspended(true);
    board_ext5v_set(false, &s_ext5v_was_on);
    s_saved_mhz = hal_cpu_mhz();
    bool slowed = hal_cpu_set_mhz(SUSPEND_CPU_MHZ);
    motion_detector_reset(&s_motion);
    kprintf("suspend: screen off, ext 5 V %s, CPU %u MHz%s\n", s_ext5v_was_on ? "off" : "was already off", hal_cpu_mhz(), slowed ? "" : " (clock change refused)");
    uint32_t irq = arch_irq_disable();
    suspend_core_entered(&s_core);
    arch_irq_restore(irq);
}

static void do_wake(void)
{
    uint64_t t0 = now_us();
    if (s_saved_mhz) hal_cpu_set_mhz(s_saved_mhz);
    if (s_ext5v_was_on) board_ext5v_set(true, NULL);
    display_panel_power(true);
    display_set_suspended(false);
    kconsole_request_refresh();                  /* full-screen programs repaint what they drew meanwhile */
    board_backlight(s_saved_backlight ? s_saved_backlight : FULL_BRIGHTNESS);
    uint32_t irq = arch_irq_disable();
    suspend_core_woke(&s_core, now_us());
    arch_irq_restore(irq);
    s_last_resume_ms = (uint32_t)((now_us() - t0) / 1000u);
    kprintf("suspend: resumed in %u ms (CPU %u MHz)\n", (unsigned)s_last_resume_ms, hal_cpu_mhz());
}

static void powerd_task(void *arg)
{
    (void)arg;
    task_sleep(IMU_START_DELAY_MS);
    if (imu_init()) kprintf("suspend: accelerometer ready (a movement wakes the device)\n");
    else            kprintf("suspend: accelerometer not available; only key and touch wake the device\n");
    for (;;) {
        uint32_t irq = arch_irq_disable();
        suspend_action_t a = suspend_core_step(&s_core, now_us());
        arch_irq_restore(irq);
        if (a == SUSPEND_ENTER) do_enter();
        else if (a == SUSPEND_WAKE) do_wake();
        else if (s_core.suspended) {
            int16_t xyz[3];
            if (imu_ready() && imu_read_accel(xyz) && motion_detector_update(&s_motion, xyz, MOTION_THRESHOLD)) {
                irq = arch_irq_disable();
                suspend_core_wake(&s_core, now_us());
                arch_irq_restore(irq);
            }
        }
        task_sleep(s_core.suspended ? SUSPENDED_POLL_MS : AWAKE_POLL_MS);
    }
}

void suspend_start(void)
{
    if (s_started) return;
    suspend_core_init(&s_core, now_us(), SUSPEND_DEFAULT_IDLE_SECONDS);
    kconsole_set_input_hook(serial_activity);
    s_started = true;
    task_create_stack("powerd", powerd_task, NULL, TASK_PRIO_DEFAULT + 2, TASK_STACK);
}

int suspend_command(int argc, char **argv)
{
    if (argc == 1) { suspend_request(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "status")) {
        kprintf("suspend: %s; auto-suspend after %u s idle (%u s left); CPU %u MHz; accelerometer %s; last resume %u ms\n",
                s_core.suspended ? "suspended" : "awake", (unsigned)s_core.idle_seconds,
                (unsigned)suspend_core_seconds_left(&s_core, now_us()), hal_cpu_mhz(),
                imu_ready() ? "ready" : "not available", (unsigned)s_last_resume_ms);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "idle")) {
        uint32_t secs = 0;
        if (!strcmp(argv[2], "off")) secs = 0;
        else {
            for (const char *p = argv[2]; *p; p++) { if (*p < '0' || *p > '9') { secs = 1; break; } secs = secs * 10 + (uint32_t)(*p - '0'); if (secs > IDLE_MAX_SECONDS) break; }
            if (secs < IDLE_MIN_SECONDS || secs > IDLE_MAX_SECONDS) { kprintf("sleep: seconds must be %u..%u, or off\n", IDLE_MIN_SECONDS, IDLE_MAX_SECONDS); return 1; }
        }
        uint32_t irq = arch_irq_disable();
        s_core.idle_seconds = secs;
        s_core.last_activity_us = now_us();      /* the new timeout counts from now */
        arch_irq_restore(irq);
        if (secs) kprintf("sleep: auto-suspend after %u s without key or touch\n", (unsigned)secs);
        else      kputs("sleep: auto-suspend off\n");
        return 0;
    }
    kputs("usage: sleep              suspend now\n       sleep status       state, timeout, CPU clock\n       sleep idle SECONDS|off   idle time before auto-suspend (10..86400)\n");
    return 1;
}
#endif
