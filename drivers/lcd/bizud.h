#pragma once
#include <m5os/types.h>
/* BIZ UD Gothic, rasterized directly at 24px; baseline supplied by caller. */
int bizud_width(uint32_t cp);
int bizud_draw(int x, int baseline, uint32_t cp, uint16_t fg, uint16_t bg);
int bizud_text(int x, int baseline, const char *text, int right, uint16_t fg, uint16_t bg);
/* OpenType vrt2/vertical-metric result baked into the atlas.  `top` is the
 * top of a 24px em cell; the returned advance is along the vertical axis. */
int bizud_vertical_advance(uint32_t cp);
int bizud_draw_vertical(int x, int top, uint32_t cp, uint16_t fg, uint16_t bg);
uint32_t bizud_decode(const char **text);
bool bizud_begin(void);
void bizud_end(void);
/* The SD atlas remains warm after the last user exits so Write -> Scheme does
 * not read 2.4 MiB again. Idle caches can be reclaimed explicitly or before
 * a large editor allocation is retried. */
size_t bizud_cache_bytes(void);
unsigned bizud_cache_users(void);
size_t bizud_trim(void);
