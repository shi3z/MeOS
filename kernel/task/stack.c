/* Single-core diagnostics: no stack pointer is retained across an IRQ unlock. */
#include "stack_internal.h"
#include "../scheduler/sched_internal.h"
#include <m5os/arch.h>
#include <m5os/errno.h>
#include <m5os/kernel.h>
#include <m5os/kprintf.h>
#include <string.h>

void task_stack_prepare(void *allocation, size_t usable)
{
    memset(allocation, TASK_STACK_GUARD, TASK_STACK_GUARD_BYTES);
    memset((uint8_t *)allocation + TASK_STACK_GUARD_BYTES, TASK_STACK_FILL, usable);
}

bool task_stack_guard_ok(task_t *t)
{
    if (!t->stack_base) return true; /* adopted boot/idle stack */
    const volatile uint8_t *p = t->stack_base;
    for (unsigned i = 0; i < TASK_STACK_GUARD_BYTES; i++)
        if (p[i] != TASK_STACK_GUARD) t->stack_guard_failed = true;
    return !t->stack_guard_failed;
}

void task_stack_check_switch(task_t *t, bool saved_sp)
{
    if (!task_stack_guard_ok(t)) kernel_panic("task stack guard damaged");
    if (saved_sp && t->stack_base) {
        uintptr_t low = (uintptr_t)t->stack_base + TASK_STACK_GUARD_BYTES;
        uintptr_t sp = (uintptr_t)t->sp;
        if (t->stack_size < arch_task_context_bytes() || sp < low ||
            sp > low + t->stack_size - arch_task_context_bytes() ||
            sp % ARCH_STACK_ALIGN) {
            /* The rejected task may differ from task_current() (the outgoing
             * task recorded by panic). Capture its identity without a lock. */
            static char detail[96];
            ksnprintf(detail,sizeof detail,"stack bounds: pid %u sp %x low %x size %u ctx %u",
                t->id,(unsigned)sp,(unsigned)low,(unsigned)t->stack_size,(unsigned)arch_task_context_bytes());
            kernel_panic(detail);
        }
    }
}

static task_t *find(uint32_t pid)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (g_tasks[i].state != TASK_UNUSED && g_tasks[i].id == pid) return &g_tasks[i];
    return NULL;
}

int task_stack_inspect(uint32_t pid, task_stack_info_t *out)
{
    if (!out) return ERR_INVAL;
    *out = (task_stack_info_t){0};
    size_t free = 0, size = 0;
    for (;;) {
        uint32_t irq = arch_irq_disable();
        task_t *t = find(pid);
        if (!t) { arch_irq_restore(irq); return ERR_NOENT; }
        if (!t->stack_base) { arch_irq_restore(irq); return ERR_NOSYS; }
        size = t->stack_size;
        bool guard_ok = task_stack_guard_ok(t);
        const volatile uint8_t *p = (uint8_t *)t->stack_base + TASK_STACK_GUARD_BYTES;
        size_t end = size - free > 128 ? free + 128 : size;
        while (free < end && p[free] == TASK_STACK_FILL) free++;
        bool done = free < end || free == size;
        if (done) *out = (task_stack_info_t){size, size - free, free, guard_ok};
        arch_irq_restore(irq);
        if (done) return ERR_OK;
        /* A tick may switch tasks here. Re-find the PID before touching its stack. */
    }
}
