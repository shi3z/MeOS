#pragma once
#include <m5os/types.h>

/*
 * Architecture boundary for the scheduler.  Implemented in hal/<chip>/.
 * The kernel never sees registers, windows or exception frames.
 */

/* Save the running context, store its stack pointer in *old_sp, and resume
 * the context whose saved stack pointer is new_sp.  Returns (much later) when
 * somebody switches back to us. */
void arch_context_switch(uint32_t **old_sp, uint32_t *new_sp);

/* Build, at the top of [stack_base, stack_base+size), a context that makes the
 * first arch_context_switch() into it start entry(arg).  If entry returns,
 * the task falls into task_exit().  Returns the initial saved sp. */
size_t arch_task_context_bytes(void); /* saved frame footprint above SP */
uint32_t *arch_task_stack_init(void *stack_base, size_t size,
                               void (*entry)(void *), void *arg);

#define ARCH_STACK_ALIGN   16u

/* ---- Interrupt control & tick (Phase 3) -------------------------------- */

/* Mask all maskable interrupts the kernel uses; returns an opaque value for
 * arch_irq_restore().  Nestable: restoring a value taken while already masked
 * leaves interrupts masked. */
uint32_t arch_irq_disable(void);
void     arch_irq_restore(uint32_t flags);

/* Sleep the CPU until the next interrupt. */
void     arch_cpu_idle(void);

/* Install the kernel's interrupt vectors and start a periodic tick.
 * handler() runs in interrupt context with interrupts masked, on the stack
 * of whichever task was interrupted, and may call arch_context_switch(). */
void     arch_tick_init(uint32_t period_us, void (*handler)(void));

/* True when interrupts are unmasked (PS.INTLEVEL == 0): i.e. we are in plain
 * task context and may block.  False inside interrupt handlers and inside
 * arch_irq_disable() sections. */
bool     arch_irq_enabled(void);
