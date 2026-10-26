#include <m5os/health.h>
#include <m5os/arch.h>
#include <m5os/peripheral.h>
#include "hal/wdt_hal.h"
#include "hal/timer_ll.h"
static wdt_hal_context_t s_wdt;
static bool s_started;
static const char s_owner;
bool hal_watchdog_start(uint32_t timeout) {
    if(timeout<100 || timeout>60000)return false;
    const peripheral_t resource=PERIPHERAL_WATCHDOG0;
    uint32_t irq=arch_irq_disable();
    if(s_started || !peripheral_claim(&resource,1,&s_owner)) {arch_irq_restore(irq);return false;}
    _timer_ll_enable_bus_clock(0,true); /* do not reset the whole timer group */
    if(mwdt_ll_check_if_enabled(&TIMERG0)) {peripheral_release_owner(&s_owner);arch_irq_restore(irq);return false;}
    /* P4 MWDT source is XTAL 40MHz; prescaler 40000 gives 1ms per tick. */
    wdt_hal_init(&s_wdt,WDT_MWDT0,40000,false);
    wdt_hal_write_protect_disable(&s_wdt);
    wdt_hal_config_stage(&s_wdt,WDT_STAGE0,timeout,WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_feed(&s_wdt);wdt_hal_enable(&s_wdt);wdt_hal_write_protect_enable(&s_wdt);
    s_started=true;arch_irq_restore(irq);return true;
}
void hal_watchdog_feed(void) {
    uint32_t irq=arch_irq_disable();
    if(s_started){wdt_hal_write_protect_disable(&s_wdt);wdt_hal_feed(&s_wdt);wdt_hal_write_protect_enable(&s_wdt);}
    arch_irq_restore(irq);
}
