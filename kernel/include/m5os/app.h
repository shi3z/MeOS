#pragma once
#include <m5os/types.h>
#include <m5os/task.h>

/*
 * Applications: a task (or a family of tasks) with an identity, a lifecycle and
 * a set of permissions.
 *
 * ISOLATION IS BY API, NOT BY HARDWARE.  The ESP32-S3 has no MMU, so every task
 * shares one address space.  Permissions are enforced by the OS interfaces an
 * app is expected to use (device_open, the M5FS adapter, task_create); an app
 * that reaches into the HAL or pokes raw addresses is NOT stopped.  This
 * catches mistakes and models a capability system; it is not a security
 * boundary.
 *
 * Lifecycle:   app_start() -> RUNNING -> EXITED (owner task returned)
 *                                    \-> KILLED (app_stop() / kill of the owner)
 * When an app ends, the kernel task `appd` reclaims what it owned: remaining
 * child tasks, open VFS handles, and memory allocated with umalloc().
 */
#define APP_PERM_CONSOLE   0x01u     /* /dev/uart    */
#define APP_PERM_DISPLAY   0x02u     /* /dev/display */
#define APP_PERM_INPUT     0x04u     /* /dev/buttons */
#define APP_PERM_FS_READ   0x08u     /* read files, list directories */
#define APP_PERM_FS_WRITE  0x10u     /* create / write / delete files, mkdir */
#define APP_PERM_SPAWN     0x20u     /* create tasks */
#define APP_PERM_IPC       0x40u     /* send / receive messages, look up tasks by name */
#define APP_PERM_ALL       0x7Fu

typedef enum { APP_FREE = 0, APP_RUNNING, APP_EXITED, APP_KILLED } app_state_t;

#define APP_NAME_MAX  TASK_NAME_MAX
#define APP_MAX       8

typedef struct app {
    uint32_t     app_id;         /* unique, never reused, > 0 */
    char         name[APP_NAME_MAX];
    app_state_t  state;
    void       (*entry)(void *);
    uint32_t     owner_task;     /* pid of the main task */
    uint32_t     permissions;
    bool         cleanup_pending;
    bool         cleaned;
    int          cleanup_error; /* last failed teardown; 0 after successful reclaim */
    uint32_t     reclaimed_bytes;
} app_t;

/* What the system knows how to start (static table now; a loader adds more in Phase 10). */
typedef struct {
    const char *name;
    void      (*entry)(void *arg);
    uint32_t    permissions;
    uint32_t    stack_size;
    uint8_t     priority;
    const char *description;
} app_def_t;

typedef struct {
    uint32_t    app_id;
    char        name[APP_NAME_MAX];
    app_state_t state;
    uint32_t    owner_task;
    uint32_t    permissions;
    uint32_t    mem_bytes;       /* umalloc'd bytes currently held */
    uint32_t    reclaimed_bytes; /* freed by the kernel when it ended */
    int         cleanup_error;
    uint32_t    tasks;           /* live tasks belonging to it */
} app_info_t;

void app_init(void);                                   /* starts the cleanup task */
int  app_start(const char *name, uint32_t *app_id);    /* 0 or -ERR; app_id receives the new id */
int  app_stop(uint32_t app_id);                        /* kill a running app; ERR_NOENT if not running */
int  app_list(app_info_t *out, int max);               /* running and recently finished apps */
const app_def_t *app_def_at(int index);                /* NULL past the end */
const app_def_t *app_def_find(const char *name);
const char *app_state_name(app_state_t s);
void app_perm_string(uint32_t perms, char out[8]);     /* "CDIrwsm"-style, '-' for missing */

/* Does the calling task's app hold `perm`?  Kernel tasks (no app) hold everything. */
bool task_has_perm(uint32_t perm);

/* Kernel-internal hook: `t` is ending (how = EXITED or KILLED). Called with interrupts masked. */
void app_task_ended(task_t *t, app_state_t how);

/* Abort a reserved/partially initialized app; failed cleanup quarantines memory. */
void app_abort(app_t *app);
