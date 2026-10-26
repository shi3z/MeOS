#pragma once
#include <stddef.h>
#include "basic.h"
/* Bounded typeahead for INPUT: cancellation polls must not eat numeric replies.
 * A single shell task owns this object. Overflows fail input rather than using
 * a truncated reply. getkey blocks and returns one byte or a negative error. */
typedef struct {
    unsigned char queue[BASIC_LINE_CAP];
    unsigned head, count;
    int overflow, after_cr;
} basic_console;
/* Returns 1 on Ctrl-C (also clears buffered typeahead), otherwise 0. */
int basic_console_feed(basic_console *, int key);
int basic_console_readline(basic_console *, char *buffer, size_t capacity,
                          int (*getkey)(void *), ubasic_output, void *opaque);
