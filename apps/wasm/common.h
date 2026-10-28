#pragma once
/* Helpers shared by the sample games. */
#include "meos.h"

static inline void put_num(char *dst, int *n, unsigned v)
{
    char t[11]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) dst[(*n)++] = t[--k];
}

/* "label: value" written on a black strip */
static inline void hud(int x, int y, const char *label, unsigned v, unsigned fg)
{
    char b[48]; int n = 0;
    while (*label && n < 30) b[n++] = *label++;
    put_num(b, &n, v);
    while (n < 14) b[n++] = ' ';
    meos_display_text(x, y, b, n, fg, 0, 2);
}

static inline void frame_wait(unsigned *last, unsigned ms)
{
    unsigned now = meos_uptime_ms();
    if (now - *last < ms) meos_sleep_ms(ms - (now - *last));
    *last = meos_uptime_ms();
}
