#pragma once
#include <m5os/types.h>

/*
 * Suspend (a pseudo sleep) for the Tab5.
 *
 * The chip's deep sleep powers the PSRAM down - the open Wi-Fi/Tailscale/SSH sessions and every program in RAM
 * would be lost - so "suspend" keeps everything running and cuts the costs it can: screen and backlight off,
 * the extension port's 5 V off, the CPU at a quarter of its clock, the input drivers polling at 20 Hz.  A key,
 * a touch or a movement of the device wakes it in a few tens of milliseconds, exactly where it was.
 */
void suspend_start(void);                       /* start the power daemon (once, after the input drivers) */
void suspend_request(void);                     /* go to sleep now (the shell command, the hot key, the Zz key) */
bool suspend_active(void);

/* Every key press and touch goes through here.  True means: the system was suspended, this input woke it, and the
 * caller must DROP it so it is not also typed into what was on the screen.  False restarts the idle timer. */
bool suspend_filter_input(void);

/* Polling loops of the input drivers: the interval to use now (slower while suspended). */
uint32_t suspend_poll_ms(uint32_t normal_ms);

/* For programs with their own frame loop (WebAssembly apps): returns at once when awake, blocks while suspended. */
void suspend_wait_if_active(void);

int suspend_command(int argc, char **argv);     /* shell: sleep [status | idle SECONDS|off] */
