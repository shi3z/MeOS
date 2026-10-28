#include <m5os/suspend_core.h>

#define US_PER_S 1000000u

void suspend_core_init(suspend_core_t *c, uint64_t now_us, uint32_t idle_seconds)
{
    *c = (suspend_core_t){ .last_activity_us = now_us, .idle_seconds = idle_seconds };
}

bool suspend_core_input(suspend_core_t *c, uint64_t now_us)
{
    if (c->suspended) { c->wake_requested = true; return true; }
    c->last_activity_us = now_us;
    return false;
}

void suspend_core_wake(suspend_core_t *c, uint64_t now_us)
{
    if (c->suspended) c->wake_requested = true;
    else c->last_activity_us = now_us;
}

void suspend_core_request_enter(suspend_core_t *c)
{
    if (!c->suspended) c->enter_requested = true;
}

suspend_action_t suspend_core_step(const suspend_core_t *c, uint64_t now_us)
{
    if (c->suspended) return c->wake_requested ? SUSPEND_WAKE : SUSPEND_NONE;
    if (c->enter_requested) return SUSPEND_ENTER;
    if (c->idle_seconds && now_us >= c->last_activity_us &&
        now_us - c->last_activity_us >= (uint64_t)c->idle_seconds * US_PER_S)
        return SUSPEND_ENTER;
    return SUSPEND_NONE;
}

void suspend_core_entered(suspend_core_t *c)
{
    c->suspended = true;
    c->enter_requested = false;
    c->wake_requested = false;
}

void suspend_core_woke(suspend_core_t *c, uint64_t now_us)
{
    c->suspended = false;
    c->wake_requested = false;
    c->last_activity_us = now_us;                       /* the idle timer starts over */
}

uint32_t suspend_core_seconds_left(const suspend_core_t *c, uint64_t now_us)
{
    if (!c->idle_seconds || c->suspended) return 0;
    uint64_t idle = now_us >= c->last_activity_us ? now_us - c->last_activity_us : 0;
    uint64_t limit = (uint64_t)c->idle_seconds * US_PER_S;
    return idle >= limit ? 0 : (uint32_t)((limit - idle + US_PER_S - 1) / US_PER_S);
}
