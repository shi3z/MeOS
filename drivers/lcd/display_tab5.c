/*
 * Tab5 display: 5" 720x1280 ILI9881C panel behind a 2-lane MIPI-DSI link, used in landscape (1280x720).
 *
 * How this differs from the SPI panels of the other boards (drivers/lcd/display.c):
 *  - There is a FRAMEBUFFER (1.8 MB RGB565, in PSRAM).  The DSI host's DMA scans it out to the panel
 *    continuously, so drawing = writing memory; nothing is "sent" per drawing call.
 *  - The CPU's data cache sits between our writes and the DMA's reads (the DMA reads PSRAM, not the
 *    cache), so every drawing call ends by writing the touched cache lines back (esp_cache_msync).
 *  - The panel is physically portrait.  Logical (x, y) in 1280x720 is stored at physical column
 *    y, row (DISPLAY_WIDTH-1 - x), i.e. the picture is rotated 90 degrees when it is written.
 *
 * Bring-up (power the DSI PHY, start the DSI bus, send the panel's init commands over DSI, start the
 * DPI video stream) uses ESP-IDF's esp_lcd DSI driver ONCE at boot, before the scheduler runs, and
 * then never again: after display_init() returns, scan-out is pure hardware.  This is the one place
 * where the kernel leans on an IDF driver for more than register headers, because the DSI PHY/host
 * bring-up is hundreds of registers of timing that has nothing to teach about an OS.
 * Panel-specific values (init sequence, video timing) are from M5Stack's board support package.
 */
#include <string.h>
#include <m5os/display.h>
#include <m5os/board.h>
#include <m5os/hal_gpio.h>
#include <m5os/hal_mem.h>
#include <m5os/kprintf.h>
#include <m5os/tab5.h>
#include "font8x8.h"
#include "bitmap_resize.h"
#include "font_ja16.h"
#include "ili9881c_tab5_init.h"
#include "st7121_tab5_init.h"

#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"

#define PANEL_W            720          /* physical, portrait */
#define PANEL_H            1280
#define BYTES_PER_PIXEL    2
#define CACHE_LINE         64u          /* ESP32-P4 L2 cache line */
#define GLYPH_SIZE         8

#define DSI_LANES          2
#define DSI_PHY_LDO_CHAN   3            /* LDO_VO3 feeds VDD_MIPI_DPHY */
#define DSI_PHY_LDO_MV     2500
#define FIRST_FRAMES_MS    200
#define SPLASH_MS          800
#define SWRESET_MS         20           /* vendor BSP waits this long after the software reset */
#define ST7121_RESET_MS    120          /* vendor BSP: wait after the software reset */
#define ST7121_CMD_GAP_MS  5            /* ... and between init commands */
#define SLEEP_OUT_MS       120          /* ILI9881C datasheet: wait after SLPOUT before further commands */

#define BACKLIGHT_PIN      22

#define ILI_CMD_PAGE_SELECT 0xFF
#define ILI_PAD_CONTROL     0xB7
#define ILI_2_LANE          0x03
#define COLMOD_RGB565       0x55

/* What differs between the two panels this board has shipped with.  Link speed and video timing are
 * the vendor BSP's values for each. */
typedef struct {
    const char *name;
    uint32_t lane_mbps, dpi_mhz;
    esp_lcd_video_timing_t timing;
} panel_info_t;

static const panel_info_t PANEL_ILI9881C = {
    "ILI9881C", 730, 60,
    { .h_size = PANEL_W, .v_size = PANEL_H, .hsync_back_porch = 140, .hsync_pulse_width = 40, .hsync_front_porch = 40,
      .vsync_back_porch = 20, .vsync_pulse_width = 4, .vsync_front_porch = 20 },
};
static const panel_info_t PANEL_ST7121 = {
    "ST7121", 965, 70,
    { .h_size = PANEL_W, .v_size = PANEL_H, .hsync_back_porch = 40, .hsync_pulse_width = 2, .hsync_front_porch = 40,
      .vsync_back_porch = 24, .vsync_pulse_width = 20, .vsync_front_porch = 200 },
};

static volatile uint32_t s_frames;       /* refresh-done interrupts seen: proves the scan-out loop runs */
static uint16_t *s_fb;                  /* physical layout: PANEL_H rows of PANEL_W pixels - what the panel scans out */
static uint16_t *s_draw;                /* where drawing goes: s_fb, or an off-screen buffer of the same layout (display_set_target) */
static esp_lcd_panel_io_handle_t s_io;

static bool on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *ev, void *ctx)
{
    (void)panel; (void)ev; (void)ctx;
    s_frames++;
    return false;
}

static void delay_ms(uint32_t ms) { while (ms--) hal_delay_us(1000); }

static int s_tx_failed;

static bool tx(uint8_t cmd, const uint8_t *data, size_t len)
{
    bool ok = esp_lcd_panel_io_tx_param(s_io, cmd, data, len) == ESP_OK;
    if (!ok) s_tx_failed++;
    return ok;
}

/* Write the cache lines covering physical rows [row0, row1] back to PSRAM so the DMA sees them. */
static void fb_flush_real(int row0, int row1);

static void fb_flush_rows(int row0, int row1)
{
    if (s_draw != s_fb) return;                       /* drawing off-screen: nothing for the DMA to see yet */
    fb_flush_real(row0, row1);
}

static void fb_flush_real(int row0, int row1)
{
    uintptr_t a = (uintptr_t)s_fb + (uintptr_t)row0 * PANEL_W * BYTES_PER_PIXEL;
    uintptr_t b = (uintptr_t)s_fb + (uintptr_t)(row1 + 1) * PANEL_W * BYTES_PER_PIXEL;
    a &= ~(uintptr_t)(CACHE_LINE - 1);
    b = (b + CACHE_LINE - 1) & ~(uintptr_t)(CACHE_LINE - 1);
    esp_cache_msync((void *)a, b - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static inline void put(int x, int y, uint16_t c) { s_draw[(size_t)(DISPLAY_WIDTH - 1 - x) * PANEL_W + y] = c; }

/* Panel start-up over DSI, before the video stream runs.  The two controllers want different sequences. */
static void panel_start_ili9881c(void)
{
    tx(LCD_CMD_SWRESET, NULL, 0);                     /* software reset: the reset line is only released, not pulsed */
    delay_ms(SWRESET_MS);
    /* Panel registers are grouped in pages; 0xFF 98 81 <n> selects page n.  The lane count lives in page 1. */
    static const uint8_t page0[] = { 0x98, 0x81, 0x00 }, page1[] = { 0x98, 0x81, 0x01 };
    static const uint8_t lanes2[] = { ILI_2_LANE }, colmod[] = { COLMOD_RGB565 }, madctl[] = { 0x00 };
    tx(ILI_CMD_PAGE_SELECT, page1, sizeof page1);
    tx(ILI_PAD_CONTROL, lanes2, 1);
    tx(ILI_CMD_PAGE_SELECT, page0, sizeof page0);
    tx(LCD_CMD_SLPOUT, NULL, 0);
    delay_ms(SLEEP_OUT_MS);
    tx(LCD_CMD_MADCTL, madctl, 1);
    tx(LCD_CMD_COLMOD, colmod, 1);
    for (size_t i = 0; i < sizeof ili9881c_tab5_init / sizeof ili9881c_tab5_init[0]; i++) {
        tx(ili9881c_tab5_init[i].cmd, ili9881c_tab5_init[i].data, ili9881c_tab5_init[i].len);
        delay_ms(ili9881c_tab5_init[i].delay_ms);
    }
}

static void panel_start_st7121(void)
{
    tx(LCD_CMD_SWRESET, NULL, 0);
    delay_ms(ST7121_RESET_MS);
    /* The table contains sleep-out and display-on with their own settle times. */
    for (size_t i = 0; i < sizeof st7121_tab5_init / sizeof st7121_tab5_init[0]; i++) {
        tx(st7121_tab5_init[i].cmd, st7121_tab5_init[i].data, st7121_tab5_init[i].len);
        delay_ms(st7121_tab5_init[i].delay_ms + ST7121_CMD_GAP_MS);
    }
}

bool display_init(void)
{
    if (!board_init()) return false;
    const panel_info_t *pi;
    switch (tab5_panel()) {
    case TAB5_PANEL_ILI9881C: pi = &PANEL_ILI9881C; break;
    case TAB5_PANEL_ST7121:   pi = &PANEL_ST7121;   break;
    default:
        kprintf("display: this Tab5 has an ST7123 panel; the driver is not written yet\n");
        return false;
    }
    kprintf("display: %s panel, %u Mbit/s per lane\n", pi->name, (unsigned)pi->lane_mbps);

    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = DSI_PHY_LDO_CHAN, .voltage_mv = DSI_PHY_LDO_MV };
    if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) != ESP_OK) { kprintf("display: DSI PHY power failed\n"); return false; }

    esp_lcd_dsi_bus_handle_t bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0, .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT, .lane_bit_rate_mbps = pi->lane_mbps,
    };
    if (esp_lcd_new_dsi_bus(&bus_cfg, &bus) != ESP_OK) { kprintf("display: DSI bus failed\n"); return false; }

    esp_lcd_dbi_io_config_t dbi_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    if (esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &s_io) != ESP_OK) { kprintf("display: DBI io failed\n"); return false; }

    if (pi == &PANEL_ST7121) panel_start_st7121(); else panel_start_ili9881c();

    /* The DPI driver takes its framebuffer from the IDF heap, which is empty (the kernel owns PSRAM).
     * Hand it the reserved top slice of PSRAM, with the same capabilities IDF itself would give PSRAM. */
    uintptr_t rb;
    size_t rs;
    hal_psram_reserve_region(&rb, &rs);
#ifndef MALLOC_CAP_SIMD
#define MALLOC_CAP_SIMD 0
#endif
    uint32_t caps[] = { MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT, 0, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_SIMD };
    if (!rs || heap_caps_add_region_with_caps(caps, (intptr_t)rb, (intptr_t)(rb + rs)) != ESP_OK) {
        kprintf("display: no PSRAM reserve for the framebuffer\n");
        return false;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0, .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = pi->dpi_mhz, .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565, .num_fbs = 1,
        .video_timing = pi->timing,
    };
    if (esp_lcd_new_panel_dpi(bus, &dpi, &panel) != ESP_OK) { kprintf("display: DPI panel failed\n"); return false; }
    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) { kprintf("display: no framebuffer\n"); return false; }
    s_fb = fb;
    s_draw = fb;
    display_clear(COLOR_BLACK);                       /* black before the stream starts */
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_refresh_done = on_refresh_done };
    esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, NULL);
    if (esp_lcd_panel_init(panel) != ESP_OK) { kprintf("display: DPI start failed\n"); return false; }
    tx(LCD_CMD_DISPON, NULL, 0);
    delay_ms(FIRST_FRAMES_MS);                        /* let a few frames go out, then check the refresh interrupt ran */
    kprintf("display: %u refresh interrupts, %d panel command(s) failed\n", (unsigned)s_frames, s_tx_failed);
    board_backlight(255);

    /* Boot splash: colour bars (red, green, blue, white), one per quarter of the width.  They also tell the
     * person at the board - who is the only one who can see the screen - whether scan-out works at all. */
    static const uint16_t bars[] = { COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE };
    for (int i = 0; i < 4; i++) display_fill_rect(i * (DISPLAY_WIDTH / 4), 0, DISPLAY_WIDTH / 4, DISPLAY_HEIGHT, bars[i]);
    delay_ms(SPLASH_MS);
    display_clear(COLOR_BLACK);
    return true;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH  - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    if (w <= 0 || h <= 0 || !s_fb) return;
    for (int i = 0; i < w; i++)
        for (int j = 0; j < h; j++) put(x + i, y + j, color);
    fb_flush_rows(DISPLAY_WIDTH - x - w, DISPLAY_WIDTH - 1 - x);
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

static void glyph_at(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
    const uint8_t *g = font8x8[c - FONT8X8_FIRST];
    int size = GLYPH_SIZE * scale;
    for (int row = 0; row < size; row++) {
        uint8_t bits = g[row / scale];
        for (int col = 0; col < size; col++) put(x + col, y + row, ((bits >> (col / scale)) & 1) ? fg : bg);
    }
}

static void glyph_cp_at(int x, int y, uint16_t cp, uint16_t fg, uint16_t bg, int scale)
{
    if (cp < 0x80u) {
        char c = (cp >= FONT8X8_FIRST && cp <= FONT8X8_LAST) ? (char)cp : ' ';
        glyph_at(x, y, c, fg, bg, scale);
        return;
    }

    uint16_t rows16[16];
    if (!font_ja16_get_glyph(cp, rows16, NULL)) {
        for (int r = 0; r < 16; r++) rows16[r] = 0;
        rows16[2] = 0x3FFCu;
        rows16[13] = 0x3FFCu;
        for (int r = 3; r < 13; r++) rows16[r] = 0x2004u;
    }

    int size = GLYPH_SIZE * scale;
    if (size == 16) {
        for (int row = 0; row < 16; row++) {
            uint16_t bits = rows16[row];
            for (int col = 0; col < 16; col++) {
                put(x + col, y + row, ((bits & (0x8000u >> col)) != 0) ? fg : bg);
            }
        }
    } else {
        for (int row = 0; row < size; row++) {
            uint16_t bits = rows16[(row * 16) / size];
            for (int col = 0; col < size; col++) {
                int src_col = (col * 16) / size;
                put(x + col, y + row, ((bits & (0x8000u >> src_col)) != 0) ? fg : bg);
            }
        }
    }
}

void display_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    int size = GLYPH_SIZE * scale;
    if (!s_fb || x < 0 || y < 0 || x + size > DISPLAY_WIDTH || y + size > DISPLAY_HEIGHT) return;
    glyph_at(x, y, c, fg, bg, scale);
    fb_flush_rows(DISPLAY_WIDTH - x - size, DISPLAY_WIDTH - 1 - x);
}

void display_draw_codepoints(int x, int y, const uint16_t *cps, int count, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    int cell = GLYPH_SIZE * scale;
    if (!s_fb || !cps || count <= 0 || x < 0 || y < 0 || y + cell > DISPLAY_HEIGHT) return;
    int len = 0;
    while (len < count && x + (len + 1) * cell <= DISPLAY_WIDTH) len++;
    if (len == 0) return;
    for (int i = 0; i < len; i++) {
        glyph_cp_at(x + i * cell, y, cps[i], fg, bg, scale);
    }
    fb_flush_rows(DISPLAY_WIDTH - x - len * cell, DISPLAY_WIDTH - 1 - x);
}

void display_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    int cell = GLYPH_SIZE * scale;
    if (!s_fb || !s || x < 0 || y < 0 || y + cell > DISPLAY_HEIGHT) return;

    uint16_t cps[160];
    int len = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && len < 160 && x + (len + 1) * cell <= DISPLAY_WIDTH) {
        uint8_t b0 = *p++;
        uint32_t cp = b0;
        if ((b0 & 0xE0u) == 0xC0u && (p[0] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(p[0] & 0x3Fu);
            p += 1;
        } else if ((b0 & 0xF0u) == 0xE0u && (p[0] & 0xC0u) == 0x80u && (p[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(p[0] & 0x3Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
            p += 2;
        } else if ((b0 & 0xF8u) == 0xF0u && (p[0] & 0xC0u) == 0x80u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
            cp = 0xFFFDu;
            p += 3;
        }
        cps[len++] = (uint16_t)(cp <= 0xFFFFu ? cp : 0xFFFDu);
    }
    if (len == 0) return;
    display_draw_codepoints(x, y, cps, len, fg, bg, scale);
}

void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *px)
{
    if (!s_fb || !px || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = x + w > DISPLAY_WIDTH  ? DISPLAY_WIDTH  - x : w;
    int y1 = y + h > DISPLAY_HEIGHT ? DISPLAY_HEIGHT - y : h;
    if (x0 >= x1 || y0 >= y1) return;

    const int TILE = 32;
    for (int jj = y0; jj < y1; jj += TILE) {
        int jmax = jj + TILE < y1 ? jj + TILE : y1;
        for (int ii = x0; ii < x1; ii += TILE) {
            int imax = ii + TILE < x1 ? ii + TILE : x1;
            for (int i = ii; i < imax; i++) {
                uint16_t *dst_col = &s_draw[(size_t)(DISPLAY_WIDTH - 1 - (x + i)) * PANEL_W + y];
                const uint16_t *src_col = &px[i];
                for (int j = jj; j < jmax; j++) {
                    dst_col[j] = src_col[(size_t)j * w];
                }
            }
        }
    }
    fb_flush_rows(DISPLAY_WIDTH - (x + x1), DISPLAY_WIDTH - 1 - (x + x0));
}

/* Off-screen drawing (double buffering): everything drawn afterwards goes to `buf`, which must be DISPLAY_BUFFER_BYTES
 * of memory (PSRAM is fine) and is laid out like the framebuffer.  display_present() copies a rectangle of it to the
 * panel in one go, so the picture never shows a half-drawn state.  NULL = draw on the screen again. */
/* Lines and triangles, for 3D.  Coordinates may lie outside the screen: everything is clipped.  Triangles are filled
 * column by column because a column is contiguous in the (rotated) framebuffer. */
static inline void put_clip(int x, int y, uint16_t c) { if ((unsigned)x < DISPLAY_WIDTH && (unsigned)y < DISPLAY_HEIGHT) put(x, y, c); }

void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!s_fb) return;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;      /* dy <= 0 */
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    if (dx > 4000 || -dy > 4000) return;                                         /* absurd: a projection blew up */
    int minx = x0 < x1 ? x0 : x1, maxx = x0 < x1 ? x1 : x0;
    for (;;) {
        put_clip(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    if (minx < 0) minx = 0;
    if (maxx >= DISPLAY_WIDTH) maxx = DISPLAY_WIDTH - 1;
    if (minx <= maxx) fb_flush_rows(DISPLAY_WIDTH - 1 - maxx, DISPLAY_WIDTH - 1 - minx);
}

void display_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    if (!s_fb) return;
    int t;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }          /* sort by x */
    if (x1 > x2) { t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (x2 - x0 > 4000 || x2 < 0 || x0 >= DISPLAY_WIDTH) return;
    int xs = x0 < 0 ? 0 : x0, xe = x2 >= DISPLAY_WIDTH ? DISPLAY_WIDTH - 1 : x2;
    for (int x = xs; x <= xe; x++) {
        /* long edge v0-v2 and the short edge on this side of v1 */
        int ya = x2 == x0 ? y0 : y0 + (int)(((int64_t)(y2 - y0) * (x - x0)) / (x2 - x0));
        int yb;
        if (x < x1) yb = x1 == x0 ? y1 : y0 + (int)(((int64_t)(y1 - y0) * (x - x0)) / (x1 - x0));
        else        yb = x2 == x1 ? y1 : y1 + (int)(((int64_t)(y2 - y1) * (x - x1)) / (x2 - x1));
        int lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
        if (lo < 0) lo = 0;
        if (hi >= DISPLAY_HEIGHT) hi = DISPLAY_HEIGHT - 1;
        uint16_t *col = &s_draw[(size_t)(DISPLAY_WIDTH - 1 - x) * PANEL_W];
        for (int y = lo; y <= hi; y++) col[y] = color;
    }
    fb_flush_rows(DISPLAY_WIDTH - 1 - xe, DISPLAY_WIDTH - 1 - xs);
}

void display_set_target(uint16_t *buf) { s_draw = buf ? buf : s_fb; }

void display_present(int x, int y, int w, int h)
{
    if (!s_fb || s_draw == s_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH  - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    int r0 = DISPLAY_WIDTH - x - w, r1 = DISPLAY_WIDTH - 1 - x;       /* physical rows of this rectangle: contiguous runs of h pixels */
    for (int r = r0; r <= r1; r++) memcpy(&s_fb[(size_t)r * PANEL_W + y], &s_draw[(size_t)r * PANEL_W + y], (size_t)h * sizeof(uint16_t));
    fb_flush_real(r0, r1);
}

void display_read_bitmap(int x, int y, int w, int h, uint16_t *out)
{
    if (!s_fb || !out || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) return;
    for (int i = 0; i < w; i++) {
        const uint16_t *src_col = &s_draw[(size_t)(DISPLAY_WIDTH - 1 - (x + i)) * PANEL_W + y];
        for (int j = 0; j < h; j++) out[(size_t)j * w + i] = src_col[j];
    }
}

void display_draw_bitmap_scaled(int x, int y, int w, int h, const uint16_t *px, int scale)
{
    if (scale <= 1) { display_draw_bitmap(x, y, w, h, px); return; }
    if (!s_fb || !px || w <= 0 || h <= 0) return;
    int dw = w * scale, dh = h * scale;
    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = x + dw > DISPLAY_WIDTH  ? DISPLAY_WIDTH  - x : dw;
    int y1 = y + dh > DISPLAY_HEIGHT ? DISPLAY_HEIGHT - y : dh;
    if (x0 >= x1 || y0 >= y1) return;

    for (int i = x0; i < x1; i++) {
        uint16_t *dst_col = &s_draw[(size_t)(DISPLAY_WIDTH - 1 - (x + i)) * PANEL_W + y];
        int sx = i / scale;
        const uint16_t *src_col = &px[sx];
        for (int j = y0; j < y1; j++) {
            dst_col[j] = src_col[(size_t)(j / scale) * w];
        }
    }
    fb_flush_rows(DISPLAY_WIDTH - (x + x1), DISPLAY_WIDTH - 1 - (x + x0));
}

/* The panel is rotated: blit contiguous columns, then flush only that span. */
void display_draw_bitmap_resized(int x,int y,int w,int h,const uint16_t *px,int dw,int dh)
{
    if (!s_fb) return;
    int first,last;
    if (tab5_bitmap_resize(s_draw,x,y,w,h,px,dw,dh,&first,&last))
        fb_flush_rows(DISPLAY_WIDTH-1-last,DISPLAY_WIDTH-1-first);
}

void display_draw_indexed_bitmap_resized(int x,int y,int w,int h,const uint8_t *indices,
    const uint16_t palette[256],int dw,int dh)
{
    if (!s_fb) return;
    int first,last;
    if (tab5_indexed_resize(s_draw,x,y,w,h,indices,palette,dw,dh,&first,&last))
        fb_flush_rows(DISPLAY_WIDTH-1-last,DISPLAY_WIDTH-1-first);
}
