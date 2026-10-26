/*
 * M5OS scheduler - Phase 3: preemptive.
 *
 * Policy: strict priority, round-robin among equals.  A task loses the CPU
 *   (a) voluntarily: task_yield(), task_sleep(), task_exit();
 *   (b) by force: every SCHED_TICK_US a timer interrupt calls sched_tick(),
 *       which makes the running task RUNNABLE and runs the same
 *       pick-and-switch as (a).  A higher-priority task that woke up gets the
 *       CPU at the next tick; equal priorities share time in 10 ms slices.
 *
 * LOCKING MODEL (single core): the scheduler's data is protected by masking
 * interrupts.  Every entry point other than sched_tick() (which runs in
 * interrupt context, already masked) brackets its work with
 * arch_irq_disable()/arch_irq_restore().  A context that is switched out
 * stays masked until it is switched back in, then restores its own level.
 *
 * The whole scheduler is two functions: sched_pick() decides, sched_switch()
 * acts.  No hidden queues: the task table IS the run queue (TASK_MAX is tiny,
 * so a linear scan is the clearest correct thing).
 */
#include <m5os/task.h>
#include <m5os/arch.h>
#include <m5os/hal_time.h>
#include <m5os/kprintf.h>
#include <m5os/kernel.h>
#include <m5os/kmalloc.h>
#include <m5os/mutex.h>
#include <m5os/errno.h>
#include <m5os/app.h>
#include <m5os/msg.h>
#include "sched_internal.h"
#include "../task/stack_internal.h"

task_t g_tasks[TASK_MAX];
static task_t *s_current;
static uint64_t s_accounted_at;

task_t *task_current(void) { return s_current; }
bool    sched_started(void) { return s_current != NULL; }

/* Wake sleepers whose time has come; free the slots of finished tasks. */
static void sched_housekeeping(void)
{
    uint64_t now = hal_time_now_us();
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = &g_tasks[i];
        if (t->state == TASK_SLEEPING && t->wakeup_us <= now) {
            t->state = TASK_RUNNABLE;
        } else if (t->state == TASK_BLOCKED && t->wait_timed && t->wakeup_us <= now) {
            t->wait_timed_out = true;               /* a timed IPC wait ran out */
            t->wait_obj = NULL;
            t->state = TASK_RUNNABLE;
        } else if (t->state == TASK_TERMINATED && t != s_current) {
            /* Safe to reclaim: nobody is running on its stack any more. */
            kfree(t->stack_base);
            t->stack_base = NULL;
            msg_free_mailbox(t);
            t->state = TASK_UNUSED;
        }
    }
}

/* Highest priority wins; among equals, strict round-robin.
 *
 * The round-robin position is kept PER PRIORITY (s_rr_last[prio] = table index
 * of the task that last ran at that priority), not "after whoever ran last".
 * The latter looks equivalent but is not: a frequently waking high-priority
 * task in a low table slot (e.g. a 20 ms poller) would reset the scan to the
 * start of the table each time and starve equal-priority tasks in later slots
 * (observed on hardware: new tasks waited up to 570 ms).
 * Idle (prio 0) is always runnable, so this never returns NULL. */
static int s_rr_last[TASK_PRIO_MAX + 1];

static task_t *sched_pick(void)
{
    int best_prio = -1;
    for (int i = 0; i < TASK_MAX; i++) {
        const task_t *t = &g_tasks[i];
        if ((t->state == TASK_RUNNABLE || t->state == TASK_RUNNING) && (int)t->priority > best_prio) {
            best_prio = t->priority;
        }
    }
    if (best_prio < 0) {
        return NULL;
    }
    int start = s_rr_last[best_prio];
    for (int n = 1; n <= TASK_MAX; n++) {
        int idx = (start + n) % TASK_MAX;
        task_t *t = &g_tasks[idx];
        if ((t->state == TASK_RUNNABLE || t->state == TASK_RUNNING) && t->priority == best_prio) {
            s_rr_last[best_prio] = idx;
            return t;
        }
    }
    return NULL;
}

/* Give the CPU away.  The caller has already set its own state
 * (RUNNABLE for yield, SLEEPING for sleep, TERMINATED for exit). */
static void sched_switch(void)
{
    task_stack_check_switch(s_current, false);
    sched_housekeeping();
    task_t *next = sched_pick();
    if (next == NULL) {
        kernel_panic("no runnable task (idle missing)");
    }
    task_t *prev = s_current;
    if (next == prev) {
        prev->state = TASK_RUNNING;
        return;
    }
    task_stack_check_switch(next, true);
    uint64_t switched_at=hal_time_now_us();
    prev->runtime_us+=switched_at-s_accounted_at;
    s_accounted_at=switched_at;
    next->state = TASK_RUNNING;
    s_current = next;
    arch_context_switch(&prev->sp, next->sp);
    /* We are back: somebody switched to us, s_current == prev again. */
}

void sched_reschedule(void) { sched_switch(); }

void task_yield(void)
{
    uint32_t irq = arch_irq_disable();
    task_t *self = s_current;
    if (self->state == TASK_RUNNING) {
        self->state = TASK_RUNNABLE;
    }
    sched_switch();
    arch_irq_restore(irq);
}

void task_sleep(uint32_t ms)
{
    uint32_t irq = arch_irq_disable();
    s_current->wakeup_us = hal_time_now_us() + (uint64_t)ms * 1000u;
    s_current->state = TASK_SLEEPING;
    sched_switch();
    arch_irq_restore(irq);
}

void task_exit(void)
{
    (void)arch_irq_disable();           /* never restored: this task is gone */
    app_task_ended(s_current, APP_EXITED);
    msg_task_ended(s_current);
    s_current->state = TASK_TERMINATED;
    mutex_release_all(s_current);
    sched_switch();
    kernel_panic("terminated task was rescheduled");
}

/* ---- preemption ---------------------------------------------------------- */

static volatile uint32_t s_ticks;

/* Runs in interrupt context (interrupts masked) on the stack of the task that
 * was interrupted.  If it switches, this call "returns" only when the
 * interrupted task is scheduled again. */
static void sched_tick(void)
{
    s_ticks++;
    task_t *self = s_current;
    if (self->state == TASK_RUNNING) {
        self->state = TASK_RUNNABLE;
    }
    sched_switch();
}

uint32_t sched_ticks(void) { return s_ticks; }

void sched_start_preemption(void)
{
    arch_tick_init(SCHED_TICK_US, sched_tick);
}

void sched_init(void)
{
    /* The code that is executing right now (kernel_main on IDF's boot stack)
     * becomes task 1, "idle".  Its sp is filled in at its first switch-out. */
    task_t *idle = &g_tasks[0];
    idle->id = 1;
    idle->name[0] = 'i'; idle->name[1] = 'd'; idle->name[2] = 'l'; idle->name[3] = 'e';
    idle->name[4] = 0;
    idle->state = TASK_RUNNING;
    idle->priority = TASK_PRIO_IDLE;
    s_current = idle;
    idle->runtime_us=0;
    s_accounted_at=hal_time_now_us();
}

/* ---- inspection & control ------------------------------------------------- */

const char *task_state_name(task_state_t st)
{
    switch (st) {
    case TASK_RUNNABLE:   return "RUNNABLE";
    case TASK_RUNNING:    return "RUNNING";
    case TASK_SLEEPING:   return "SLEEPING";
    case TASK_BLOCKED:    return "BLOCKED";
    case TASK_TERMINATED: return "TERMINATED";
    default:              return "?";
    }
}

int task_snapshot(task_info_t *out, int max)
{
    int n = 0;
    uint32_t irq = arch_irq_disable();
    for (int i = 0; i < TASK_MAX && n < max; i++) {
        const task_t *t = &g_tasks[i];
        if (t->state == TASK_UNUSED) continue;
        out[n].id = t->id;
        for (int k = 0; k < TASK_NAME_MAX; k++) out[n].name[k] = t->name[k];
        out[n].state = t->state;
        out[n].priority = t->priority;
        out[n].base_priority = t->base_priority;
        out[n].stack_size = (uint32_t)t->stack_size;
        out[n].app_id = t->app ? t->app->app_id : 0;
        n++;
    }
    arch_irq_restore(irq);
    return n;
}
uint64_t task_current_runtime_us(void)
{
    uint32_t irq=arch_irq_disable();
    uint64_t total=s_current?s_current->runtime_us+hal_time_now_us()-s_accounted_at:0;
    arch_irq_restore(irq);return total;
}
int task_runtime_snapshot(task_runtime_info_t *out,int max)
{
    if(!out||max<=0)return 0;
    uint32_t irq=arch_irq_disable();uint64_t at=hal_time_now_us();int n=0;
    for(int i=0;i<TASK_MAX&&n<max;i++){
        const task_t *t=&g_tasks[i];if(t->state==TASK_UNUSED)continue;
        out[n].id=t->id;
        for(int j=0;j<TASK_NAME_MAX;j++)out[n].name[j]=t->name[j];
        out[n].runtime_us=t->runtime_us+(t==s_current?at-s_accounted_at:0);n++;
    }
    arch_irq_restore(irq);return n;
}

int task_kill(uint32_t pid)
{
    uint32_t irq = arch_irq_disable();
    int rc = ERR_NOENT;
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = &g_tasks[i];
        if (t->state == TASK_UNUSED || t->state == TASK_TERMINATED || t->id != pid) continue;
        if (t == s_current || t->id == 1) { rc = ERR_INVAL; break; }
        app_task_ended(t, APP_KILLED);
        msg_task_ended(t);
        t->state = TASK_TERMINATED;
        mutex_release_all(t);               /* remove donation before handing off owned locks */
        t->wait_obj = NULL;
        t->state = TASK_TERMINATED;         /* the stack is freed by the next scheduler pass */
        rc = ERR_OK;
        break;
    }
    arch_irq_restore(irq);
    return rc;
}

int task_kill_app(const struct app *a)
{
    int n = 0;
    uint32_t irq = arch_irq_disable();
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = &g_tasks[i];
        if (t->app != a || t == s_current) continue;
        if (t->state == TASK_UNUSED || t->state == TASK_TERMINATED) continue;
        t->state = TASK_TERMINATED;
        mutex_release_all(t);
        msg_task_ended(t);
        t->wait_obj = NULL;
        t->state = TASK_TERMINATED;
        n++;
    }
    arch_irq_restore(irq);
    return n;
}
