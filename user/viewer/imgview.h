#pragma once
#include <m5os/types.h>

/*
 * The one image module: decoder, scaled drawing and the full-screen viewer.
 *
 * Formats: BMP (uncompressed 8/16/24/32-bit, bottom-up or top-down, RGB565 bit-fields) and PPM/PGM (P6 / P5,
 * 8-bit).  JPEG/PNG are not supported (no decoder in the tree).  Everything is held as RGB565.
 */
bool imgview_is_image_name(const char *name);                       /* .bmp / .ppm / .pgm, any case */
const char *imgview_last_error(void);                               /* why the last load/show failed */

/* Decode at full size into a kmalloc_psram'd RGB565 buffer (the caller kfree()s it). */
bool imgview_load(const char *path, uint16_t **pixels, int *w, int *h);

/* Draw src (src_w x src_h) scaled to dst_w x dst_h at (x, y), nearest neighbour. */
void imgview_draw_fit(int x, int y, int dst_w, int dst_h, const uint16_t *src, int src_w, int src_h);

/* Write an RGB565 image as a 24-bit BMP.  Returns 0 or a negative ERR_*. */
int imgview_write_bmp(const char *path, const uint16_t *px, int w, int h);

/* The viewer.  nav (may be NULL) lets a file browser add "next/previous image" and "open the camera":
 *   step(dir, path, cap)  dir = +1 / -1: replace path with the neighbouring image; false if there is none.
 *   camera()              the user pressed P: the viewer closes, then camera() runs. */
typedef struct {
    bool (*step)(int dir, char *path, size_t cap);
    void (*camera)(void);
} imgview_nav_t;

/* Shows the image and waits for the user to close it.  A message window is shown if it cannot be decoded. */
bool imgview_show(const char *path, const imgview_nav_t *nav);
