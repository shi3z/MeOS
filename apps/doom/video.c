#include <m5os/board_config.h>
#if M5OS_DOOM
#include <m5os/display.h>
#include "doomgeneric.h"
#include "i_video.h"
#include "v_video.h"
#include "runtime.h"
#include "tables.h"
/* Only these two upstream i_video.c definitions are renamed at compile time.
 * Preserve palette/gamma bookkeeping, while presenting the indexed image
 * directly instead of expanding to RGB888 and converting it back to RGB565. */
void doom_engine_set_palette(byte *palette);
uint16_t doom_video_palette[256];
void I_SetPalette(byte *palette)
{
    doom_engine_set_palette(palette);
    for (unsigned i=0;i<256;i++) {
        unsigned r=gammatable[usegamma][palette[i*3]], g=gammatable[usegamma][palette[i*3+1]], b=gammatable[usegamma][palette[i*3+2]];
        doom_video_palette[i]=RGB565(r,g,b);
    }
}
void I_FinishUpdate(void) { DG_DrawFrame(); }
#endif
