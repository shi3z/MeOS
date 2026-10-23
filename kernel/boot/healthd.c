#include <m5os/health.h>
#include <m5os/task.h>
#include <m5os/hal_time.h>
#include <m5os/kernel.h>
#include <m5os/kprintf.h>
#ifndef M5OS_WATCHDOG_ENABLED
#define M5OS_WATCHDOG_ENABLED 0
#endif
static bool s_armed;
static void supervisor(void *unused) {
    (void)unused;uint32_t last=sched_ticks();
    for(;;) {
        task_sleep(100);
        uint32_t ticks=sched_ticks(),bad=health_expired();
        if(M5OS_WATCHDOG_ENABLED) {
            if(!s_armed) {
                if(!hal_watchdog_start(2000))kernel_panic("watchdog unavailable or already owned");
                s_armed=true;
            }
            if(bad) {
                health_info_t leases[HEALTH_MAX];char reason[80];
                unsigned n=health_snapshot(leases,HEALTH_MAX);
                ksnprintf(reason,sizeof reason,"health lease %u expired",bad);
                for(unsigned i=0;i<n;i++)if(leases[i].handle==bad)
                    ksnprintf(reason,sizeof reason,"health %s lease %u owner %u expired",leases[i].name,bad,leases[i].owner);
                kernel_panic(reason);
            }
            if(ticks!=last)hal_watchdog_feed(); /* never feed from the timer interrupt */
        }
        last=ticks;
    }
}
void health_start(void) {
    if(task_create_for_app("healthd",supervisor,NULL,TASK_PRIO_MAX,4096,NULL)<0)
        kernel_panic("cannot create health supervisor");
}
int health_command(int argc,char **argv) {
    (void)argc;(void)argv;health_info_t leases[HEALTH_MAX];unsigned n=health_snapshot(leases,HEALTH_MAX);
    uint64_t now=hal_time_now_us();uint32_t bad=health_expired();
    kprintf("watchdog: %s; stale lease %u (0=none)\n",s_armed?"armed":"disabled",bad);
    for(unsigned i=0;i<n;i++)kprintf("%u %s owner %u timeout %u ms age %u ms\n",leases[i].handle,leases[i].name,
        leases[i].owner,leases[i].timeout_ms,now>=leases[i].last_us?(unsigned)((now-leases[i].last_us)/1000):0);
    return 0;
}
