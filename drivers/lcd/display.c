#include <m5os/display.h>
#include <m5os/board.h>
#include <m5os/hal_lcd_bus.h>
#include <m5os/bus.h>
#include <m5os/kprintf.h>
#include "panel.h"
#include "font8x8.h"

#define GLYPH_SIZE  8
#define BACKLIGHT_FULL  255

bool display_is_suspended(void) { return false; }          /* only the Tab5 can suspend */

bool display_init(void)
{
    if (!board_init()) {
        return false;
    }
    panel_init();
    display_clear(COLOR_BLACK);          /* clear GRAM before the backlight comes on */
    board_backlight(BACKLIGHT_FULL);
    return true;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH  - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    spi_bus_lock();
    hal_lcd_begin();
    panel_set_window(x, y, x + w - 1, y + h - 1);
    hal_lcd_write_pixels(color, (uint32_t)w * (uint32_t)h);
    hal_lcd_end();
    spi_bus_unlock();
}

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    display_fill_rect(x, y, w, 1, color);
    display_fill_rect(x, y + h - 1, w, 1, color);
    display_fill_rect(x, y, 1, h, color);
    display_fill_rect(x + w - 1, y, 1, h, color);
}

void display_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
    if (scale < 1) scale = 1;
    int size = GLYPH_SIZE * scale;
    if (x < 0 || y < 0 || x + size > DISPLAY_WIDTH || y + size > DISPLAY_HEIGHT) return;

    const uint8_t *glyph = font8x8[c - FONT8X8_FIRST];
    spi_bus_lock();
    hal_lcd_begin();
    panel_set_window(x, y, x + size - 1, y + size - 1);
    for (int row = 0; row < size; row++) {
        uint8_t bits = glyph[row / scale];
        for (int col = 0; col < size; col++) {
            hal_lcd_write_pixel(((bits >> (col / scale)) & 1) ? fg : bg);
        }
    }
    hal_lcd_end();
    spi_bus_unlock();
}

void display_draw_codepoints(int x, int y, const uint16_t *cps, int count, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    int cell = GLYPH_SIZE * scale;
    if (!cps || count <= 0 || x < 0 || y < 0 || y + cell > DISPLAY_HEIGHT) return;
    int len = 0;
    while (len < count && x + (len + 1) * cell <= DISPLAY_WIDTH) len++;
    if (len == 0) return;

    spi_bus_lock();
    hal_lcd_begin();
    panel_set_window(x, y, x + len * cell - 1, y + cell - 1);
    for (int row = 0; row < cell; row++) {
        for (int i = 0; i < len; i++) {
            uint16_t cp = cps[i];
            char c = (cp >= FONT8X8_FIRST && cp <= FONT8X8_LAST) ? (char)cp : '?';
            uint8_t bits = font8x8[c - FONT8X8_FIRST][row / scale];
            for (int col = 0; col < cell; col++) hal_lcd_write_pixel(((bits >> (col / scale)) & 1) ? fg : bg);
        }
    }
    hal_lcd_end();
    spi_bus_unlock();
}

/* One window and one bus transaction for the whole string (it used to be one per character,
 * which made full-screen text slow).  Characters that would not fit are cut off. */
void display_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    int cell = GLYPH_SIZE * scale;
    if (x < 0 || y < 0 || y + cell > DISPLAY_HEIGHT) return;
    int len = 0;
    while (s[len] && x + (len + 1) * cell <= DISPLAY_WIDTH) len++;
    if (len == 0) return;

    spi_bus_lock();
    hal_lcd_begin();
    panel_set_window(x, y, x + len * cell - 1, y + cell - 1);
    for (int row = 0; row < cell; row++) {
        for (int i = 0; i < len; i++) {
            char c = s[i];
            if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
            uint8_t bits = font8x8[c - FONT8X8_FIRST][row / scale];
            for (int col = 0; col < cell; col++) hal_lcd_write_pixel(((bits >> (col / scale)) & 1) ? fg : bg);
        }
    }
    hal_lcd_end();
    spi_bus_unlock();
}

void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *px)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) return;
    spi_bus_lock();
    hal_lcd_begin();
    panel_set_window(x, y, x + w - 1, y + h - 1);
    for (int i = 0; i < w * h; i++) hal_lcd_write_pixel(px[i]);
    hal_lcd_end();
    spi_bus_unlock();
}

void display_draw_bitmap_scaled(int x, int y, int w, int h, const uint16_t *px, int scale)
{
    if (scale <= 1) { display_draw_bitmap(x, y, w, h, px); return; }
    int dw = w * scale, dh = h * scale;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + dw > DISPLAY_WIDTH || y + dh > DISPLAY_HEIGHT) return;
    spi_bus_lock();
    hal_lcd_begin();
    panel_set_window(x, y, x + dw - 1, y + dh - 1);
    for (int j = 0; j < dh; j++) {
        const uint16_t *row = &px[(j / scale) * w];
        for (int i = 0; i < dw; i++) hal_lcd_write_pixel(row[i / scale]);
    }
    hal_lcd_end();
    spi_bus_unlock();
}
