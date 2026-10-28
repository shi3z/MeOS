#include <assert.h>
#include <stdio.h>
#include <m5os/suspend_core.h>

#define S 1000000ull

int main(void)
{
    suspend_core_t c;

    /* idle timeout */
    suspend_core_init(&c, 0, 180);
    assert(suspend_core_step(&c, 179 * S) == SUSPEND_NONE);
    assert(suspend_core_step(&c, 180 * S) == SUSPEND_ENTER);
    assert(suspend_core_seconds_left(&c, 0) == 180 && suspend_core_seconds_left(&c, 100 * S) == 80);
    assert(suspend_core_seconds_left(&c, 180 * S) == 0);

    /* input restarts the timer and is not consumed while awake */
    assert(!suspend_core_input(&c, 100 * S));
    assert(suspend_core_step(&c, 279 * S) == SUSPEND_NONE);
    assert(suspend_core_step(&c, 280 * S) == SUSPEND_ENTER);

    /* disabled */
    suspend_core_init(&c, 0, 0);
    assert(suspend_core_step(&c, 100000 * S) == SUSPEND_NONE);
    assert(suspend_core_seconds_left(&c, 5 * S) == 0);
    c.idle_seconds = 60;                                  /* enabling later counts from the last activity */
    assert(suspend_core_step(&c, 59 * S) == SUSPEND_NONE && suspend_core_step(&c, 60 * S) == SUSPEND_ENTER);

    /* explicit request, even though nothing is idle */
    suspend_core_init(&c, 0, 180);
    suspend_core_request_enter(&c);
    assert(suspend_core_step(&c, 1 * S) == SUSPEND_ENTER);
    suspend_core_entered(&c);
    assert(c.suspended && !c.enter_requested);
    assert(suspend_core_step(&c, 100000 * S) == SUSPEND_NONE);   /* nothing to do while asleep */
    suspend_core_request_enter(&c);                               /* ignored while already asleep */
    assert(!c.enter_requested);

    /* the input that wakes the system is swallowed, and so is everything until the wake-up is done */
    assert(suspend_core_input(&c, 50 * S));
    assert(suspend_core_step(&c, 50 * S) == SUSPEND_WAKE);
    assert(suspend_core_input(&c, 50 * S + 10000));                   /* a key repeat before the screen is back */
    suspend_core_woke(&c, 51 * S);
    assert(!c.suspended && !c.wake_requested && c.last_activity_us == 51 * S);
    assert(!suspend_core_input(&c, 52 * S));                          /* afterwards input is normal again */
    assert(suspend_core_step(&c, 51 * S + 179 * S) == SUSPEND_NONE);  /* the timer restarted at the last activity (52 s) */
    assert(suspend_core_step(&c, 52 * S + 180 * S) == SUSPEND_ENTER);

    /* wake by something that is not a key (serial) */
    suspend_core_init(&c, 0, 180);
    suspend_core_entered(&c);
    suspend_core_wake(&c, 10 * S);
    assert(suspend_core_step(&c, 10 * S) == SUSPEND_WAKE);
    suspend_core_woke(&c, 11 * S);
    suspend_core_wake(&c, 20 * S);                                    /* awake: just activity */
    assert(!c.wake_requested && c.last_activity_us == 20 * S);

    /* a clock that goes backwards must not suspend (or underflow) */
    suspend_core_init(&c, 100 * S, 180);
    assert(suspend_core_step(&c, 50 * S) == SUSPEND_NONE);
    assert(suspend_core_seconds_left(&c, 50 * S) == 180);

    puts("suspend: idle timer, requests, consumed wake input, wake sources, clock safety PASS");
    return 0;
}
