#pragma once
#include <m5os/types.h>

#define OTFONT_DEFAULT_PATH "/assets/fonts/BIZUDGothic-Regular.ttf"

typedef struct {
    uint32_t font_bytes;
    uint32_t resident_bytes;
    uint32_t load_ms;
    uint32_t glyph_hits;
    uint32_t glyph_misses;
    uint32_t metric_hits;
    uint32_t metric_misses;
    uint32_t io_bytes;
    uint32_t io_reads;
    uint64_t raster_us;
    size_t cache_bytes;
} otfont_stats;

/* Experimental runtime OpenType renderer.  A bounded VFS stream cache and the
 * 4-bit glyph cache live in PSRAM; callers still own display serialization. */
bool otfont_begin(const char *path, unsigned pixel_size);
/* Prefer a PSRAM-backed face, falling back to the streamed face when the full
 * font cannot be allocated or read. */
bool otfont_begin_resident(const char *path, unsigned pixel_size);
bool otfont_set_pixel_size(unsigned pixel_size);
void otfont_end(void);
int otfont_width(uint32_t cp);
int otfont_draw(int x, int baseline, uint32_t cp, uint16_t fg, uint16_t bg);
int otfont_vertical_advance(uint32_t cp);
int otfont_draw_vertical(int x, int top, uint32_t cp, uint16_t fg, uint16_t bg);
int otfont_text(int x, int baseline, const char *text, int right,
                uint16_t fg, uint16_t bg);
void otfont_get_stats(otfont_stats *out);

