/* Declares only the console permission, then tries what it must not be able to do. */
#include "meos.h"

static void num(const char *label, int v)
{
    char b[48];
    int n = 0;
    while (*label) b[n++] = *label++;
    b[n++] = '=';
    if (v < 0) { b[n++] = '-'; v = -v; }
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[n++] = t[--k];
    b[n++] = '\n';
    meos_console_write(b, n);
}

void _start(void)
{
    num("display_fill (no D, want -16)", meos_display_fill(0, 0, 100, 100, 0xF800));
    num("file_open (no r, want -16)", meos_file_open("/", 1, 1));
    num("console_write bad ptr", meos_console_write((const char *)0x7FFFFFF0, 64));   /* traps: the app dies, the system does not */
    meos_puts("not reached\n");
}
