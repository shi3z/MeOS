#include <m5os/task.h>
#include <m5os/arch.h>
#include <m5os/kprintf.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/app.h>
#include <m5os/errno.h>
#include "../scheduler/sched_internal.h"
#include "stack_internal.h"

/* Stacks come from the internal-SRAM heap (never PSRAM: they are touched in
 * interrupt context and must not depend on the external-RAM cache).  Blocks are
 * 16-byte aligned, which the Xtensa ABI requires at every call boundary.
 * They are freed by the scheduler when a TERMINATED task's slot is reclaimed. */
static uint32_t s_next_pid = 2;      /* pid 1 is idle */

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    for (; i < TASK_NAME_MAX - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

int task_create(const char *name, void (*entry)(void *), void *arg, uint8_t priority)
{
    return task_create_stack(name, entry, arg, priority, TASK_STACK_SIZE);
}

int task_create_stack(const char *name, void (*entry)(void *), void *arg, uint8_t priority, size_t stack_size)
{
    /* A task created by an application belongs to the same application. */
    task_t *cur = task_current();
    return task_create_for_app(name, entry, arg, priority, stack_size, cur ? cur->app : NULL);
}

int task_create_for_app(const char *name, void (*entry)(void *), void *arg, uint8_t priority, size_t stack_size, struct app *app)
{
    if (!task_has_perm(APP_PERM_SPAWN)) {
        return ERR_PERM;                           /* checked against the CALLER's app, not the new task's */
    }
    if (!entry || priority > TASK_PRIO_MAX || stack_size < TASK_STACK_MIN ||
        stack_size > (size_t)-1 - TASK_STACK_GUARD_BYTES) {
        return -1;
    }
    stack_size &= ~(size_t)(ARCH_STACK_ALIGN - 1);
    void *stack = kmalloc_internal(stack_size + TASK_STACK_GUARD_BYTES);   /* before masking interrupts */
    if (!stack) {
        return -1;
    }
    task_stack_prepare(stack, stack_size);
    uint32_t irq = arch_irq_disable();             /* slot search + fill is not atomic */
    for (int i = 1; i < TASK_MAX; i++) {          /* slot 0 is idle */
        task_t *t = &g_tasks[i];
        if (t->state != TASK_UNUSED) {
            continue;
        }
        t->id         = s_next_pid++;
        copy_name(t->name, name);
        t->priority   = priority;
        t->entry      = entry;
        t->arg        = arg;
        t->stack_base = stack;
        t->stack_size = stack_size;
        t->stack_guard_failed = false;
        t->app        = app;
        t->held       = NULL;
        t->wait_obj   = NULL;
        t->mbox       = NULL;
        t->wait_kind  = 0;
        t->wait_timed = false;
        t->wait_timed_out = false;
        t->wakeup_us  = 0;
        t->sp         = arch_task_stack_init((uint8_t *)t->stack_base + TASK_STACK_GUARD_BYTES,
                                              t->stack_size, entry, arg);
        t->state      = TASK_RUNNABLE;            /* last: now visible to the scheduler */
        arch_irq_restore(irq);
        return (int)t->id;
    }
    arch_irq_restore(irq);
    kfree(stack);                                  /* no free TCB slot */
    return -1;
}

void task_ps(void)
{
    kprintf("PID NAME            PRI STATE\n");
    for (int i = 0; i < TASK_MAX; i++) {
        const task_t *t = &g_tasks[i];
        if (t->state == TASK_UNUSED) continue;
        kprintf("%-3u %-15s %-3u %s\n", t->id, t->name, t->priority, task_state_name(t->state));
    }
}
