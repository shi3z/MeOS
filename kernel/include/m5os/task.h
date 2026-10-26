#pragma once
#include <m5os/types.h>

/*
 * Task control block and task API.
 *
 * A "task" is a thread of control with its own stack inside the single
 * address space.  There is no process isolation (see README).
 */

typedef enum {
    TASK_UNUSED = 0,      /* free TCB slot (internal; not a real state)        */
    TASK_RUNNABLE,        /* ready, waiting for the CPU                         */
    TASK_RUNNING,         /* owns the CPU right now (exactly one)               */
    TASK_SLEEPING,        /* waiting for wakeup_us                              */
    TASK_BLOCKED,         /* waiting for an event (IPC, device, ...)            */
    TASK_TERMINATED,      /* finished; slot is reclaimed by the scheduler       */
} task_state_t;

#define TASK_NAME_MAX      16
#define TASK_MAX           16
#define TASK_STACK_SIZE    4096u
#define TASK_STACK_GUARD_BYTES 32u
#define TASK_STACK_MIN     512u
#define TASK_PRIO_IDLE     0
#define TASK_PRIO_DEFAULT  1
#define TASK_PRIO_MAX      7

struct mutex;
struct app;
struct mailbox;

typedef struct task {
    uint32_t     *sp;            /* MUST stay first: saved stack pointer      */
    uint32_t      id;            /* pid, 1-based, never reused                */
    char          name[TASK_NAME_MAX];
    task_state_t  state;
    uint8_t       base_priority; /* configured priority, before mutex donation */
    uint8_t       priority;      /* higher number = more urgent               */
    uint64_t      wakeup_us;     /* valid while SLEEPING                      */
    void        (*entry)(void *);
    void         *arg;
    struct app   *app;           /* owning application, NULL for kernel tasks (children inherit) */
    struct mutex *wait_mutex; /* distinct from mailbox wait_obj */
    struct mutex *held;          /* mutexes this task owns (so task_kill can release them) */
    void         *wait_obj;      /* what a BLOCKED task waits for (a mutex or a mailbox) */
    struct mailbox *mbox;        /* this task's message queue; created on first use, freed when the task is reclaimed */
    uint8_t       wait_kind;     /* IPC: why it is blocked on a mailbox (receive / send) */
    bool          wait_timed;    /* BLOCKED with a deadline in wakeup_us */
    bool          wait_timed_out;
    void         *stack_base;
    size_t        stack_size;  /* usable bytes, excludes the low-end guard */
    bool          stack_guard_failed; /* latched until this TCB is reused */
    uint64_t      runtime_us;   /* scheduled wall time; includes ISR/kernel work */
} task_t;

/* Create a task; returns its pid (>0) or -1. */
int      task_create(const char *name, void (*entry)(void *), void *arg, uint8_t priority);
int      task_create_for_app(const char *name, void (*entry)(void *), void *arg, uint8_t priority, size_t stack_size, struct app *app);
int      task_create_stack(const char *name, void (*entry)(void *), void *arg, uint8_t priority, size_t stack_size);
void     task_yield(void);
void     task_sleep(uint32_t ms);
void     task_exit(void) __attribute__((noreturn));
task_t  *task_current(void);
void     task_ps(void);          /* print the task table */

/* Scheduler internals, exposed so they are not hidden (see sched.c). */
void     sched_init(void);       /* adopts the boot context as pid 1 "idle"   */

/* ---- Preemption (Phase 3) ---------------------------------------------- */
#define SCHED_TICK_US      10000u      /* 10 ms time slice / timer resolution */
void     sched_start_preemption(void); /* start the periodic tick             */
uint32_t sched_ticks(void);            /* ticks since preemption started      */

/* Used by blocking primitives (mutex): the caller, with interrupts masked, sets
 * its own state (BLOCKED) and calls sched_reschedule() to give the CPU away. */
void     sched_reschedule(void);
bool     sched_started(void);          /* true once sched_init() has run */

/* ---- inspection & control (used by the shell) -------------------------- */
typedef struct {
    uint32_t     id;
    char         name[TASK_NAME_MAX];
    task_state_t state;
    uint8_t      priority;
    uint8_t      base_priority;
    uint32_t     stack_size;
    uint32_t     app_id;         /* 0 = kernel task */
} task_info_t;

int         task_snapshot(task_info_t *out, int max);   /* live tasks, copied under a short critical section */
uint64_t    task_current_runtime_us(void); /* scheduled wall time including IRQs */
typedef struct {
    uint32_t id;
    char name[TASK_NAME_MAX];
    uint64_t runtime_us;
} task_runtime_info_t;
int task_runtime_snapshot(task_runtime_info_t *out,int max); /* existing snapshot ABI unchanged */
const char *task_state_name(task_state_t s);
/* Forcibly terminate a task.  Any mutex it owned is released (and handed to a
 * waiter) so the system cannot deadlock on a killed owner; the data that mutex
 * protected may be half-updated.  Not allowed on idle (pid 1) or on oneself.
 * Returns ERR_NOENT / ERR_INVAL accordingly. */
int         task_kill(uint32_t pid);

int         task_kill_app(const struct app *a);          /* kill every live task of an app (not the caller); returns count */

/* On-demand painted-stack estimate, scanned in <=128-byte critical sections.
 * ERR_NOSYS: idle uses the uninstrumented boot stack. ERR_NOENT: task vanished.
 * Not an atomic snapshot of a running task; fill-pattern matches can hide use. */
typedef struct {
    size_t size, used, free;
    bool guard_ok;
} task_stack_info_t;
int task_stack_inspect(uint32_t pid, task_stack_info_t *out);
