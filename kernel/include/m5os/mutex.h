#pragma once
#include <m5os/types.h>
#include <m5os/task.h>

/*
 * Blocking, non-recursive mutex with direct hand-off.
 *
 * A task that finds the mutex taken becomes TASK_BLOCKED (wait_obj = the
 * mutex) and is not scheduled again until mutex_unlock() makes it the owner.
 * Priority inheritance propagates through mutex wait chains; configured
 * priority is restored as waiters/ownership change.
 *
 * Rules: task context only, never from an interrupt handler or with
 * interrupts masked.  The idle task (pid 1) must never block, so on
 * contention it spins with task_yield() instead.
 */
typedef struct mutex {
    task_t       *owner;
    struct mutex *next_held;     /* link in owner->held */
} mutex_t;

#define MUTEX_INIT   { 0, 0 }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* Kernel-internal: release every mutex owned by `t` (task_kill). Interrupts must be masked. */
void mutex_release_all(task_t *t);
void mutex_recompute_priorities(void); /* kernel-internal, IRQs masked */
