#include <m5os/crash.h>
#include <m5os/kernel.h>
#include <m5os/hal_console.h>
#include <m5os/hal_time.h>
#include <m5os/arch.h>
#include <m5os/kprintf.h>
#include "esp_system.h"
#ifndef M5OS_BUILD_ID
#define M5OS_BUILD_ID "unknown"
#endif
/* Internal noinit DRAM, best effort across software/watchdog resets only. */
static crash_record_t s_retained __attribute__((section(".noinit")));
static crash_record_t s_previous;
static bool s_have_previous, s_panicking;
static void raw(const char *s) { for(unsigned i=0;s && s[i] && i<256;i++) hal_console_putc(s[i]); }
static void number(uint32_t n) {
    const char hex[]="0123456789abcdef";
    raw("0x"); for(int i=7;i>=0;i--) hal_console_putc(hex[(n>>(i*4))&15]);
}
void crash_boot_report(void)
{
    esp_reset_reason_t reset=esp_reset_reason();
    s_have_previous=reset!=ESP_RST_POWERON && reset!=ESP_RST_BROWNOUT && crash_record_valid(&s_retained);
    if(s_have_previous) s_previous=s_retained;
    s_retained.magic=0; /* consume: a later unrelated reset must not replay this panic */
    kprintf("Build: %s; reset reason: %u\n",M5OS_BUILD_ID,(unsigned)reset);
    crash_print_previous();
}
void crash_print_previous(void)
{
    if(!s_have_previous) { kputs("Previous MeOS panic: unavailable\n"); return; }
    kprintf("Previous MeOS panic: %s; build %s; task %u (%s), app %u\n",
        s_previous.reason,s_previous.build,s_previous.task_id,s_previous.task_name,s_previous.app_id);
    kprintf("Saved SP %x; usable stack %u; guard damage latched %u; uptime seconds %u\n",
        s_previous.saved_sp,s_previous.stack_bytes,s_previous.guard_failed,(unsigned)(s_previous.uptime_us/1000000));
}
void kernel_panic(const char *reason)
{
    (void)arch_irq_disable();
    if(!s_panicking) {
        s_panicking=true;
        crash_record_make(&s_retained,task_current(),hal_time_now_us(),M5OS_BUILD_ID,reason);
        /* No console mutex, allocator, VFS, framebuffer or stack scan on the panic path. */
        raw("\nMeOS PANIC: "); raw(s_retained.reason); raw("\nbuild "); raw(s_retained.build);
        raw(" task "); number(s_retained.task_id); raw(" saved SP "); number(s_retained.saved_sp); raw("\n");
        hal_console_flush();
    }
    for(;;) { } /* a separately enabled hardware watchdog may reset the system */
}
