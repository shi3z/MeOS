/*
 * Interrupt masking, idle and the periodic tick for the ESP32-P4 (RISC-V, CLIC).
 * Only CPU CSRs, the CLIC registers and the SYSTIMER are used: no esp_intr_alloc (which is
 * FreeRTOS-aware) and no esp_timer.
 */
#include <m5os/arch.h>
#include "hal/systimer_ll.h"
#include "hal/interrupt_clic_ll.h"
#include "soc/systimer_struct.h"
#include "soc/interrupts.h"
#include "soc/clic_reg.h"
#include "soc/soc.h"
#include "esp_intr_alloc.h"
#include <m5os/kprintf.h>

#define TICK_LINE        20u                       /* CPU interrupt line 0..23 are free for applications */
#define CLIC_ID          (TICK_LINE + 16u)         /* external lines are CLIC interrupt numbers 16..47 */
#define TICK_LEVEL       1u                        /* above threshold 0 */
#define SYSTIMER_COUNTER 0
#define SYSTIMER_ALARM   0
#define TICKS_PER_US     16u
#define MSTATUS_MIE      8u
#define CSR_MTVT         0x307

extern uint64_t hal_time_ticks(void);
extern const uint32_t m5_mtvt_table[];
extern void m5_tick_entry(void);

static void (*s_handler)(void);
static uint64_t s_next;
static uint32_t s_period;

uint32_t arch_irq_disable(void)
{
    uint32_t old;
    __asm__ volatile ("csrrci %0, mstatus, 8" : "=r"(old) :: "memory");   /* read-and-clear MIE, atomically */
    return old;
}

void arch_irq_restore(uint32_t flags)
{
    if (flags & MSTATUS_MIE) __asm__ volatile ("csrsi mstatus, 8" ::: "memory");
}

bool arch_irq_enabled(void)
{
    uint32_t m;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(m));
    return (m & MSTATUS_MIE) != 0;
}

void arch_cpu_idle(void)
{
    /* MIE on, then wait: the next interrupt (the tick at the latest) wakes the core and is taken. */
    __asm__ volatile ("csrsi mstatus, 8\n\twfi" ::: "memory");
}

static void arm(uint64_t target)
{
    systimer_ll_set_alarm_target(&SYSTIMER, SYSTIMER_ALARM, target);
    systimer_ll_apply_alarm_value(&SYSTIMER, SYSTIMER_ALARM);
    systimer_ll_enable_alarm(&SYSTIMER, SYSTIMER_ALARM, true);
}

/* Called from m5_tick_entry (vectors_p4.S) with interrupts masked. */
void m5_tick_dispatch(void)
{
    /* Re-arm relative to the previous target (no drift); if we are late, restart from now. */
    s_next += s_period;
    uint64_t now = hal_time_ticks();
    if ((int64_t)(s_next - now) <= 0) s_next = now + s_period;
    arm(s_next);
    systimer_ll_clear_alarm_int(&SYSTIMER, SYSTIMER_ALARM);
    if (s_handler) s_handler();                  /* may switch tasks; returns when this task runs again */
}

void arch_tick_init(uint32_t period_us, void (*handler)(void))
{
    /* Tell IDF's interrupt allocator the tick line is taken, so a driver's esp_intr_alloc() never picks it. */
    if (esp_intr_reserve(TICK_LINE, 0) != ESP_OK) kprintf("irq: CPU line %u was already in use by IDF\n", (unsigned)TICK_LINE);
    uint32_t flags = arch_irq_disable();
    __asm__ volatile("csrs mstatus, %0" :: "r"(0x6000u) : "memory"); /* FS=Dirty before any tick FPU save */
    s_handler = handler;
    s_period = period_us * TICKS_PER_US;

    /* Our vector table instead of IDF's (see vectors_p4.S). */
    if (m5_mtvt_table[CLIC_ID] != (uint32_t)(uintptr_t)m5_tick_entry) {     /* table (vectors_p4.S) and CLIC_ID must agree */
        for (;;) { }
    }
    __asm__ volatile ("csrw %0, %1" :: "i"(CSR_MTVT), "r"((uintptr_t)m5_mtvt_table));
    __asm__ volatile ("fence" ::: "memory");

    /* SYSTIMER alarm 0 (one-shot, re-armed by hand) -> interrupt matrix -> CPU line -> CLIC. */
    systimer_ll_connect_alarm_counter(&SYSTIMER, SYSTIMER_ALARM, SYSTIMER_COUNTER);
    systimer_ll_enable_alarm_oneshot(&SYSTIMER, SYSTIMER_ALARM);
    systimer_ll_enable_alarm_int(&SYSTIMER, SYSTIMER_ALARM, true);
    s_next = hal_time_ticks() + s_period;
    arm(s_next);

    interrupt_clic_ll_route(0, ETS_SYSTIMER_TARGET0_INTR_SOURCE, CLIC_ID);
    REG_SET_FIELD(CLIC_INT_THRESH_REG, CLIC_CPU_INT_THRESH, 0);
    uint32_t ctl = (TICK_LEVEL << (8 - NLBITS)) | ((1u << (8 - NLBITS)) - 1u);     /* level in the top bits, rest all ones */
    REG_SET_FIELD(CLIC_INT_CTRL_REG(CLIC_ID), CLIC_INT_CTL, ctl);
    REG_CLR_BIT(CLIC_INT_CTRL_REG(CLIC_ID), 1u << CLIC_INT_ATTR_TRIG_S);            /* trig[0] = 0: level-triggered */
    REG_SET_BIT(CLIC_INT_CTRL_REG(CLIC_ID), CLIC_INT_ATTR_SHV);                     /* hardware-vectored: jump via MTVT */
    REG_SET_BIT(CLIC_INT_CTRL_REG(CLIC_ID), CLIC_INT_IE);

    arch_irq_restore(flags);
}
