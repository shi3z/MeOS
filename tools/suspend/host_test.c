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

    /* wake by something that is not a key (motion, serial) */
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

    /* the motion detector */
    {
        motion_detector_t m = { 0 };
        const int32_t thr = 1024;                                      /* about a quarter of a g at +-8 g */
        int16_t flat[3] = { 12, -8, 4096 }, flat2[3] = { 15, -5, 4099 }, tilted[3] = { 4096, 10, 20 }, bump[3] = { 300, 200, 5200 };
        assert(!motion_detector_update(&m, flat, thr));                /* the first sample only sets the baseline */
        assert(!motion_detector_update(&m, flat2, thr));               /* sensor noise */
        assert(!motion_detector_update(&m, flat, thr));
        assert(motion_detector_update(&m, tilted, thr));               /* picked up and turned over */
        assert(!motion_detector_update(&m, tilted, thr));              /* held still in the new pose */
        assert(motion_detector_update(&m, bump, thr));
        motion_detector_reset(&m);
        assert(!motion_detector_update(&m, bump, thr));                /* a fresh baseline after a reset */
        int16_t edge[3] = { 4096 + 1024, 0, 0 };
        motion_detector_reset(&m);
        int16_t base[3] = { 4096, 0, 0 };
        motion_detector_update(&m, base, thr);
        assert(!motion_detector_update(&m, edge, thr));                /* exactly the threshold does not trip */
        int16_t over[3] = { 4096 + 1025, 0, 0 };
        motion_detector_update(&m, base, thr);
        assert(motion_detector_update(&m, over, thr));
    }
    puts("suspend: idle timer, requests, consumed wake input, wake sources, clock safety and motion PASS");
    return 0;
}
