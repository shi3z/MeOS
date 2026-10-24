/* MeOS system implementation, replacing the upstream desktop i_system.c. */
#include "doomtype.h"
#include "i_system.h"
#include "i_video.h"
#include "i_joystick.h"
#include "i_sound.h"
#include <string.h>

void I_AtExit(atexit_func_t fn, boolean run_on_error) { (void)fn; (void)run_on_error; }
void I_Tactile(int on, int off, int total) { (void)on; (void)off; (void)total; }
byte *I_ZoneBase(int *size)
{
    *size = 6 * 1024 * 1024;
    byte *p = malloc(*size);
    if (!p) I_Error("Doom: no PSRAM for 6 MiB zone");
    return p;
}
boolean I_ConsoleStdout(void) { return false; }
void I_Init(void) { }
void I_BindVariables(void) { I_BindVideoVariables(); I_BindJoystickVariables(); I_BindSoundVariables(); }
void I_PrintBanner(char *s) { puts(s); }
void I_PrintDivider(void) { puts("--------------------------------"); }
void I_PrintStartupBanner(char *s) { puts(s); }
void I_Quit(void) { exit(0); }
void I_Error(char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    puts(""); exit(1);
}
boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    (void)offset;
    if (size != 1 && size != 2 && size != 4) return false;
    memset(value, 0, size); return true;
}
