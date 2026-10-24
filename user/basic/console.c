#include "console.h"
#include <string.h>

int basic_console_feed(basic_console *c, int key)
{
    if (key < 0) return 0;
    if (key == 3) { memset(c, 0, sizeof *c); return 1; }
    if (c->count == sizeof c->queue) c->overflow = 1;
    else {
        c->queue[(c->head+c->count) % sizeof c->queue] = (unsigned char)key;
        c->count++;
    }
    return 0;
}
static void emit(ubasic_output write, void *opaque, const char *s, size_t len)
{
    if (write) write(s, len, opaque);
}
int basic_console_readline(basic_console *c, char *buffer, size_t cap,
                          int (*getkey)(void *), ubasic_output write, void *opaque)
{
    if (!cap) return -2;
    buffer[0] = 0;
    if (c->overflow) { memset(c, 0, sizeof *c); return -2; }
    size_t len = 0;
    int invalid = 0;
    for (;;) {
        int key;
        if (c->count) {
            key = c->queue[c->head]; c->head = (c->head+1) % sizeof c->queue; c->count--;
        } else key = getkey(opaque);
        if (key < 0) return -2;
        int was_cr = c->after_cr; c->after_cr = 0;
        if (key == '\n' && was_cr) continue;
        if (key == 3) {
            memset(c, 0, sizeof *c); buffer[0] = 0;
            emit(write, opaque, "^C\n", 3); return -1;
        }
        if (key == '\r' || key == '\n') {
            c->after_cr = key == '\r'; buffer[len] = 0;
            emit(write, opaque, "\n", 1); return invalid ? 1 : 0;
        }
        if (key == 8 || key == 127) {
            if (len) { len--; buffer[len] = 0; emit(write, opaque, "\b \b", 3); }
            continue;
        }
        if (key == 21) {
            while (len) { len--; emit(write, opaque, "\b \b", 3); }
            buffer[0] = 0; invalid = 0; continue;
        }
        if (key < 32 && key != '\t') { invalid = 1; continue; }
        if (len + 1 >= cap) { invalid = 1; continue; }
        buffer[len++] = (char)key; buffer[len] = 0;
        char ch = (char)key; emit(write, opaque, &ch, 1);
    }
}
