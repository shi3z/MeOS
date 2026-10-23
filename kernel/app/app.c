#include <m5os/wasm.h>
#include <m5os/app.h>
#include <m5os/arch.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/vfs.h>
#include <m5os/kprintf.h>
#include <m5os/gui.h>
#if M5OS_BOARD_TAB5
#include <m5os/audio.h>
#endif
#include "../scheduler/sched_internal.h"

const app_def_t *apps_def_table(int *count);          /* apps/apps.c */

#define APPD_PERIOD_MS  20u
#define APPD_STACK      4096u
#define APPD_PRIO       (TASK_PRIO_DEFAULT + 2)

static app_t    s_apps[APP_MAX];
static uint32_t s_next_id = 1;

/* ------------------------------------------------------------- permissions */

bool task_has_perm(uint32_t perm)
{
    task_t *t = task_current();
    if (!t || !t->app) return true;                   /* kernel tasks are trusted */
    return (t->app->permissions & perm) == perm;
}

void app_perm_string(uint32_t p, char out[8])
{
    static const char letters[7] = { 'C', 'D', 'I', 'r', 'w', 's', 'm' };
    for (int i = 0; i < 7; i++) out[i] = (p & (1u << i)) ? letters[i] : '-';
    out[7] = 0;
}

const char *app_state_name(app_state_t s)
{
    switch (s) {
    case APP_RUNNING: return "RUNNING";
    case APP_EXITED:  return "EXITED";
    case APP_KILLED:  return "KILLED";
    default:          return "FREE";
    }
}

/* -------------------------------------------------------------- definitions */

const app_def_t *app_def_at(int index)
{
    int n;
    const app_def_t *t = apps_def_table(&n);
    return (index >= 0 && index < n) ? &t[index] : NULL;
}

static bool str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

const app_def_t *app_def_find(const char *name)
{
    for (int i = 0; app_def_at(i); i++) if (str_eq(app_def_at(i)->name, name)) return app_def_at(i);
    return NULL;
}

/* --------------------------------------------------------------- lifecycle */

/* Called by the scheduler (interrupts masked) whenever a task ends.  Only the
 * owner task decides the app's fate; the heavy cleanup is left to appd. */
void app_task_ended(task_t *t, app_state_t how)
{
    app_t *a = t->app;
    if (a && t->id == a->owner_task && a->state == APP_RUNNING) {
        a->state = how;
        a->cleanup_pending = true;
    }
}

static app_t *find_slot(void)
{
    app_t *oldest = NULL;
    for (int i = 0; i < APP_MAX; i++) {
        app_t *a = &s_apps[i];
        if (a->state == APP_FREE) return a;
        if (a->state != APP_RUNNING && a->cleaned && (!oldest || a->app_id < oldest->app_id)) oldest = a;
    }
    return oldest;                                    /* recycle the oldest finished app */
}

/* Starting an app is three steps so that a loader can prepare memory tagged with the
 * new app's id BEFORE the task exists:
 *   app_reserve()  take a slot and an id (state RUNNING, no task yet)
 *   ... caller allocates / loads, owner = a->app_id ...
 *   app_launch()   create the task (it belongs to the app); or app_abort() to give up. */
app_t *app_reserve(const char *name, uint32_t permissions)
{
    uint32_t irq = arch_irq_disable();
    app_t *a = find_slot();
    if (a) {
        *a = (app_t){ .app_id = s_next_id++, .state = APP_RUNNING, .permissions = permissions };
        for (int i = 0; name[i] && i < APP_NAME_MAX - 1; i++) a->name[i] = name[i];
    }
    arch_irq_restore(irq);
    return a;
}

int app_launch(app_t *a, void (*task_entry)(void *), void *arg, uint8_t priority, size_t stack_size)
{
    uint32_t irq = arch_irq_disable();             /* the task and the app's owner_task appear together */
    int pid = task_create_for_app(a->name, task_entry, arg, priority, stack_size, a);
    if (pid < 0) {
        a->state = APP_FREE;
        arch_irq_restore(irq);
        kmem_reclaim_owner(a->app_id);             /* whatever the loader allocated for it */
        return pid;
    }
    a->entry = task_entry;
    a->owner_task = (uint32_t)pid;
    arch_irq_restore(irq);
    return ERR_OK;
}

void app_abort(app_t *a)
{
    kmem_reclaim_owner(a->app_id);
    uint32_t irq = arch_irq_disable();
    a->state = APP_FREE;
    arch_irq_restore(irq);
}

int app_start(const char *name, uint32_t *app_id)
{
    const app_def_t *def = app_def_find(name);
    if (!def) return ERR_NOENT;
    app_t *a = app_reserve(def->name, def->permissions);
    if (!a) return ERR_BUSY;
    int rc = app_launch(a, def->entry, NULL, def->priority, def->stack_size);
    if (rc == ERR_OK && app_id) *app_id = a->app_id;
    return rc;
}

int app_stop(uint32_t app_id)
{
    uint32_t owner = 0;
    uint32_t irq = arch_irq_disable();
    for (int i = 0; i < APP_MAX; i++)
        if (s_apps[i].state == APP_RUNNING && s_apps[i].app_id == app_id) owner = s_apps[i].owner_task;
    arch_irq_restore(irq);
    if (!owner) return ERR_NOENT;
    return task_kill(owner);                          /* -> app_task_ended(KILLED); appd does the rest */
}

int app_list(app_info_t *out, int max)
{
    task_info_t ti[TASK_MAX];
    int nt = task_snapshot(ti, TASK_MAX);
    int n = 0;
    for (int i = 0; i < APP_MAX && n < max; i++) {
        const app_t *a = &s_apps[i];
        if (a->state == APP_FREE) continue;
        app_info_t *o = &out[n++];
        o->app_id = a->app_id;
        for (int k = 0; k < APP_NAME_MAX; k++) o->name[k] = a->name[k];
        o->state = a->state;
        o->owner_task = a->owner_task;
        o->permissions = a->permissions;
        o->mem_bytes = (uint32_t)kmem_owned_bytes(a->app_id);
        o->reclaimed_bytes = a->reclaimed_bytes;
        o->tasks = 0;
        for (int t = 0; t < nt; t++) if (ti[t].app_id == a->app_id && ti[t].state != TASK_TERMINATED) o->tasks++;
    }
    return n;
}

/* ------------------------------------------------------------------- appd */

/* Kernel task: when an app ends, take back everything it still owns.  This is
 * done here (task context) because closing handles takes mutexes, which the
 * scheduler hook (interrupts masked) could not do. */
static void appd_task(void *arg)
{
    (void)arg;
    for (;;) {
        task_sleep(APPD_PERIOD_MS);
        for (int i = 0; i < APP_MAX; i++) {
            app_t *a = &s_apps[i];
            uint32_t irq = arch_irq_disable();
            bool work = a->cleanup_pending && a->state != APP_RUNNING;
            if (work) a->cleanup_pending = false;
            arch_irq_restore(irq);
            if (!work) continue;

            task_kill_app(a);                          /* children still running */
            gui_release_owner(a->app_id);              /* its windows */
            vfs_close_owned(a->app_id);                /* handles left open */
#if M5OS_BOARD_TAB5
            audio_close(a->app_id);                 /* stop callbacks before reclaiming app memory */
#endif
            wasm_app_cleanup(a->app_id);               /* the screen, if a WebAssembly app still held it */
            a->reclaimed_bytes = (uint32_t)kmem_reclaim_owner(a->app_id);   /* umalloc'd memory */
            a->cleaned = true;
        }
    }
}

void app_init(void)
{
    task_create_for_app("appd", appd_task, NULL, APPD_PRIO, APPD_STACK, NULL);
}
