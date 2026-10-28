#pragma once
#include <m5os/types.h>
#include <m5os/power.h>

/*
 * Battery level and remaining time, derived from one power_read() sample plus a short history.
 *
 * The level is the coarse voltage-only estimate of power.c (neither a fuel gauge nor proof that a
 * battery is fitted).  The remaining time is   charge left / discharge current,   where the current is a
 * running average: the instantaneous value swings with every Wi-Fi burst and screen update, and a time
 * computed from it would jump around.  While the battery is not discharging (external power, idle)
 * there is no meaningful remaining time and none is shown.
 */
#define BATTERY_TAU_MS            60000u     /* time constant of the current average */
#define BATTERY_DISCHARGE_MIN_UA  30000      /* below this net discharge the time is indeterminate (30 mA) */
#define BATTERY_CHARGE_MIN_UA     30000      /* above this net charge the battery counts as charging */
#define BATTERY_MAX_MINUTES       (99 * 60 + 59)

typedef struct { int32_t avg_ua; bool seeded; uint64_t last_us; } battery_tracker_t;

typedef struct {
    bool valid;            /* a usable sample and a plausible pack voltage */
    int  percent;          /* 0..100 */
    bool charging;
    int  minutes;          /* remaining minutes while discharging; -1 = not known / not discharging */
} battery_status_t;

void battery_tracker_update(battery_tracker_t *t, int32_t battery_ua, uint64_t now_us);
void battery_status_make(const battery_tracker_t *t, const power_sample_t *s, uint32_t capacity_mah, battery_status_t *out);
/* "Bat 95% ~2h05m", "Bat 95% charging", "Bat 95%", or "Bat --".  Returns the length written. */
int  battery_format(const battery_status_t *st, char *out, size_t cap);
