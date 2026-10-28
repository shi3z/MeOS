#include <m5os/battery.h>
#include <m5os/kprintf.h>

void battery_tracker_update(battery_tracker_t *t, int32_t ua, uint64_t now_us)
{
    if (!t->seeded) { t->avg_ua = ua; t->seeded = true; t->last_us = now_us; return; }
    uint64_t dt_ms = (now_us - t->last_us) / 1000u;
    t->last_us = now_us;
    if (dt_ms > BATTERY_TAU_MS * 4u) dt_ms = BATTERY_TAU_MS * 4u;           /* a long gap (suspend): do not trust the old average */
    /* avg += (ua - avg) * dt / (tau + dt): exponential average with time constant tau, valid for any dt */
    int64_t delta = (int64_t)ua - t->avg_ua;
    t->avg_ua += (int32_t)(delta * (int64_t)dt_ms / (int64_t)(BATTERY_TAU_MS + dt_ms));
}

void battery_status_make(const battery_tracker_t *t, const power_sample_t *s, uint32_t capacity_mah, battery_status_t *out)
{
    *out = (battery_status_t){ .valid = false, .percent = 0, .charging = false, .minutes = -1 };
    if (s->result != POWER_OK || s->estimated_percent < 0) return;
    out->valid = true;
    out->percent = s->estimated_percent;
    int32_t avg = t->seeded ? t->avg_ua : s->battery_ua;
    if (avg > BATTERY_CHARGE_MIN_UA) { out->charging = true; return; }
    if (avg < -BATTERY_DISCHARGE_MIN_UA) {
        int64_t left_uah = (int64_t)capacity_mah * 1000 * out->percent / 100;       /* charge left, in microamp-hours */
        int64_t minutes = left_uah * 60 / -(int64_t)avg;
        out->minutes = minutes > BATTERY_MAX_MINUTES ? BATTERY_MAX_MINUTES : (int)minutes;
    }
}

int battery_format(const battery_status_t *st, char *out, size_t cap)
{
    if (!st->valid) return ksnprintf(out, cap, "Bat --");
    if (st->charging) return ksnprintf(out, cap, "Bat %d%% charging", st->percent);
    if (st->minutes < 0) return ksnprintf(out, cap, "Bat %d%%", st->percent);
    if (st->minutes >= BATTERY_MAX_MINUTES) return ksnprintf(out, cap, "Bat %d%% >99h", st->percent);
    if (st->minutes >= 60) return ksnprintf(out, cap, "Bat %d%% ~%dh%02dm", st->percent, st->minutes / 60, st->minutes % 60);
    return ksnprintf(out, cap, "Bat %d%% ~%dm", st->percent, st->minutes);
}
