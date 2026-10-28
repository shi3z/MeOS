#pragma once
#include <m5os/types.h>

/* Boundary between portable kernel and chip-specific code.
 * A port to another SoC re-implements hal/<chip>/ only. */
void hal_console_init(void);
void hal_console_putc(char c);          /* blocking with timeout, never hangs */
void hal_console_flush(void);
int  hal_console_getc(void);            /* -1 if no byte pending              */
