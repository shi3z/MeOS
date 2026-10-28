/* Host regression for kernel/lib/battery.c.  ksnprintf is mapped to the host's snprintf: the formats used
 * (%d, %02d, %%) mean the same in both. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <m5os/battery.h>

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

static power_sample_t sample(int percent, int32_t ua)
{
    power_sample_t s = { .result = POWER_OK, .battery_ua = ua, .battery_mv = 7800, .estimated_percent = percent };
    return s;
}
static const char *text(int percent, int32_t avg_ua, uint32_t mah)
{
    static char out[40];
    battery_tracker_t t = { .avg_ua = avg_ua, .seeded = true };
    power_sample_t s = sample(percent, avg_ua);
    battery_status_t st;
    battery_status_make(&t, &s, mah, &st);
    battery_format(&st, out, sizeof out);
    return out;
}
#define EXPECT(got, want) do { if (strcmp((got), (want))) { printf("FAIL: got \"%s\", want \"%s\"\n", (got), (want)); assert(0); } } while (0)

int main(void)
{
    EXPECT(text(50, -500000, 2000), "Bat 50% ~2h00m");        /* 1000 mAh left at 500 mA */
    EXPECT(text(95, -250000, 2000), "Bat 95% ~7h36m");        /* 1900 mAh at 250 mA = 456 min */
    EXPECT(text(10, -1500000, 2000), "Bat 10% ~8m");          /* 200 mAh at 1.5 A */
    EXPECT(text(0, -500000, 2000), "Bat 0% ~0m");
    EXPECT(text(95, -500, 2000), "Bat 95%");                  /* 0.5 mA: external power / idle: no time */
    EXPECT(text(95, -30000, 2000), "Bat 95%");                /* exactly the threshold is not enough */
    EXPECT(text(95, -30001, 2000), "Bat 95% ~63h19m");        /* 1900 mAh / 30.001 mA = 3799.9 min, rounded down */
    EXPECT(text(80, 800000, 2000), "Bat 80% charging");
    EXPECT(text(80, 30000, 2000), "Bat 80%");
    EXPECT(text(100, -31000, 20000), "Bat 100% >99h");        /* capped */
    {   /* unusable samples */
        battery_tracker_t t = { 0 };
        battery_status_t st; char out[40];
        power_sample_t bad = sample(50, -500000);
        bad.result = POWER_NO_RESPONSE;
        battery_status_make(&t, &bad, 2000, &st); battery_format(&st, out, sizeof out); EXPECT(out, "Bat --");
        power_sample_t nopack = sample(-1, -500000);        /* voltage outside any plausible 2S range */
        battery_status_make(&t, &nopack, 2000, &st); battery_format(&st, out, sizeof out); EXPECT(out, "Bat --");
    }
    {   /* the average: first sample seeds it, later ones move it by dt / (tau + dt) */
        battery_tracker_t t = { 0 };
        battery_tracker_update(&t, -500000, 0);
        assert(t.avg_ua == -500000);
        battery_tracker_update(&t, -100000, 60u * 1000000u);    /* dt == tau: halfway */
        assert(t.avg_ua == -300000);
        battery_tracker_update(&t, -300000, 70u * 1000000u);    /* already equal: unchanged */
        assert(t.avg_ua == -300000);
        battery_tracker_t u = { 0 };
        battery_tracker_update(&u, -500000, 0);
        battery_tracker_update(&u, -100000, 600u * 1000000u);   /* a 10 minute gap counts as 4 tau: 80% of the way */
        assert(u.avg_ua == -180000);
        battery_tracker_t v = { 0 };
        battery_tracker_update(&v, -500000, 0);
        for (int i = 1; i <= 50; i++) battery_tracker_update(&v, 0, (uint64_t)i * 1000000u);   /* 50 s of idle after a heavy load */
        assert(v.avg_ua > -250000 && v.avg_ua < -150000);       /* decays smoothly, does not snap */
    }
    {   /* the average, not the instantaneous current, drives the time */
        battery_tracker_t t = { .avg_ua = -250000, .seeded = true };
        power_sample_t s = sample(50, -2000000);                /* a spike in this one sample */
        battery_status_t st; char out[40];
        battery_status_make(&t, &s, 2000, &st); battery_format(&st, out, sizeof out);
        EXPECT(out, "Bat 50% ~4h00m");
    }
    puts("battery: level, remaining time, charging, thresholds, cap and averaging PASS");
    return 0;
}
