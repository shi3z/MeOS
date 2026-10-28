#pragma once
#include <m5os/types.h>

/* Image viewer: BMP (uncompressed 8/16/24/32-bit) and PPM/PGM (P6 / P5, 8-bit).  Decodes into an RGB565
 * buffer scaled to fit the screen, shows it centred on black, waits for a key.  JPEG/PNG are not
 * supported (no decoder in the tree).  Returns false and shows a message if the file cannot be shown. */
bool imgview_show(const char *path);

const char *imgview_last_error(void);     /* why the last imgview_decode()/imgview_show() failed */

/* Decode only: returns a kmalloc_psram'd RGB565 buffer of *w x *h (caller kfree()s it), or NULL. */
uint16_t *imgview_decode(const char *path, int max_w, int max_h, int *w, int *h);

/* Write an RGB565 image as a 24-bit BMP.  Returns 0 or a negative ERR_*. */
int imgview_write_bmp(const char *path, const uint16_t *px, int w, int h);
