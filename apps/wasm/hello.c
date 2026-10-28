#include "meos.h"

void _start(void)
{
    meos_puts("こんにちは、WebAssembly!\n");
    for (int i = 1; i <= 3; i++) {
        char b[] = "tick 0\n";
        b[5] = (char)('0' + i);
        meos_puts(b);
        meos_sleep_ms(300);
    }
}
