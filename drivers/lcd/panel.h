#pragma once
/* The LCD controller, as display.c sees it.  One implementation per panel:
 *   ili9342.c  CoreS3 (320x240) */
void panel_init(void);
void panel_set_window(int x0, int y0, int x1, int y1);   /* inclusive, in display coordinates; then stream pixels */
