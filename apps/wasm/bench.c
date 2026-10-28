/* How fast is the interpreter?  Integer loop, recursive calls and memory traffic, timed with uptime_ms(). */
#include "meos.h"

static void num(const char *label, unsigned v)
{
    char b[64]; int n = 0;
    while (*label) b[n++] = *label++;
    b[n++] = '=';
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[n++] = t[--k];
    b[n++] = '\n';
    meos_console_write(b, n);
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static unsigned char buf[16384];

void _start(void)
{
    unsigned t = meos_uptime_ms();
    volatile unsigned acc = 0;
    for (unsigned i = 0; i < 3000000; i++) acc += i ^ (acc >> 3);
    num("loop 3M iterations, ms", meos_uptime_ms() - t);

    t = meos_uptime_ms();
    int f = fib(24);
    num("fib(24) (75k calls), ms", meos_uptime_ms() - t);
    num("fib result", (unsigned)f);

    t = meos_uptime_ms();
    for (int r = 0; r < 100; r++) for (int i = 0; i < 16384; i++) buf[i] = (unsigned char)(buf[i] + i + r);
    num("memory 1.6M byte writes, ms", meos_uptime_ms() - t);
}
