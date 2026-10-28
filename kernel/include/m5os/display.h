#pragma once
#include <m5os/types.h>

#include <m5os/board_config.h>
#define DISPLAY_WIDTH    M5OS_DISPLAY_W
#define DISPLAY_HEIGHT   M5OS_DISPLAY_H

#define RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR_BLACK      RGB565(0, 0, 0)
#define COLOR_WHITE      RGB565(255, 255, 255)
#define COLOR_RED        RGB565(255, 0, 0)
#define COLOR_GREEN      RGB565(0, 255, 0)
#define COLOR_BLUE       RGB565(0, 0, 255)
#define COLOR_YELLOW     RGB565(255, 255, 0)
#define COLOR_CYAN       RGB565(0, 255, 255)
#define COLOR_GRAY       RGB565(128, 128, 128)

/* Phase-12 style API.  No framebuffer: drawing streams straight to the panel,
 * so concurrent callers must be serialized (a display lock arrives with the
 * device layer in Phase 5). */
bool display_init(void);
void display_clear(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_rect(int x, int y, int w, int h, uint16_t color);   /* outline */
void display_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void display_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
void display_draw_codepoints(int x, int y, const uint16_t *cps, int count, uint16_t fg, uint16_t bg, int scale);
void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *rgb565);
void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color);                          /* Tab5 */
void display_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);   /* Tab5 */
#define DISPLAY_BUFFER_BYTES ((size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t))
void display_set_target(uint16_t *buf);     /* Tab5: draw into an off-screen buffer of DISPLAY_BUFFER_BYTES (NULL: the screen again) */
void display_present(int x, int y, int w, int h);   /* Tab5: copy a rectangle of that buffer to the screen */
void display_read_bitmap(int x, int y, int w, int h, uint16_t *rgb565_out);   /* Tab5: copy of the screen, to put back with display_draw_bitmap */
void display_draw_bitmap_scaled(int x, int y, int w, int h, const uint16_t *rgb565, int scale);
