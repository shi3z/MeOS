#pragma once
#include <m5os/types.h>

/* CPU clock (Tab5).  Only the clocks that keep the PLL running are offered: the PLL also feeds other blocks, and
 * stopping it would take them down with the CPU. */
unsigned hal_cpu_mhz(void);                 /* the clock the CPU runs at now */
bool     hal_cpu_set_mhz(unsigned mhz);     /* false if this chip does not have that setting */
