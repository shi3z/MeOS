#pragma once
#include <m5os/types.h>

/*
 * The decisions of the suspend feature, with no hardware in them: when to go to sleep, when to wake, which
 * input is swallowed.  kernel/boot/suspend.c drives the hardware from these answers; tools/suspend tests
 * them on the host.
 *
 * Time is passed in (microseconds, monotonic) so the tests can run it as fast as they like.
 */
#define SUSPEND_DEFAULT_IDLE_SECONDS 180u

typedef struct {
    uint64_t last_activity_us;
    uint32_t idle_seconds;          /* 0 = never suspend on idleness */
    bool     suspended;
    bool     enter_requested;       /* `sleep`, the hot key, the on-screen Zz key */
    bool     wake_requested;
} suspend_core_t;

typedef enum { SUSPEND_NONE = 0, SUSPEND_ENTER, SUSPEND_WAKE } suspend_action_t;

void suspend_core_init(suspend_core_t *c, uint64_t now_us, uint32_t idle_seconds);

/* A key or touch arrived.  While suspended it asks for a wake-up and returns true: the caller must DROP the input,
 * so the key that woke the screen is not also typed into whatever was on it.  Otherwise it restarts the idle
 * timer and returns false. */
bool suspend_core_input(suspend_core_t *c, uint64_t now_us);

/* Activity that must wake the system but is not a key press to swallow (serial input). */
void suspend_core_wake(suspend_core_t *c, uint64_t now_us);

void suspend_core_request_enter(suspend_core_t *c);

/* One look at the state.  The caller performs the returned action and then reports it with _entered / _woke. */
suspend_action_t suspend_core_step(const suspend_core_t *c, uint64_t now_us);
void suspend_core_entered(suspend_core_t *c);
void suspend_core_woke(suspend_core_t *c, uint64_t now_us);

/* Seconds of inactivity left before an automatic suspend; 0 if disabled or already due. */
uint32_t suspend_core_seconds_left(const suspend_core_t *c, uint64_t now_us);
