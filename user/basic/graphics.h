#pragma once
#include "basic.h"
typedef struct { int active; uint16_t background; } basic_graphics;
/* Tab5 implementation, explicit unavailable stub elsewhere. The owner must call
 * SCREEN 0 at every execution exit, before destroying the context. */
const char *basic_graphics_command(basic_graphics *, int op, const int32_t args[5],
                                   const char *text, size_t length,
                                   int (*cancel)(void *), void *opaque);
