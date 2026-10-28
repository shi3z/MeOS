/* Asks for more memory than its manifest allows: the host must refuse. */
#include "meos.h"

void _start(void)
{
    int pages = 0;
    while (__builtin_wasm_memory_grow(0, 1) != -1 && pages < 200) pages++;      /* 64 KB per page */
    char b[] = "grew 000 pages\n";
    b[5] = (char)('0' + pages / 100); b[6] = (char)('0' + pages / 10 % 10); b[7] = (char)('0' + pages % 10);
    meos_puts(b);
}
