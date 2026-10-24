#pragma once
#include <m5os/types.h>
/* BIZ UD Gothic, rasterized directly at 24px; baseline supplied by caller. */
int bizud_width(uint32_t cp);
int bizud_draw(int x, int baseline, uint32_t cp, uint16_t fg, uint16_t bg);
int bizud_text(int x, int baseline, const char *text, int right, uint16_t fg, uint16_t bg);
uint32_t bizud_decode(const char **text);
bool bizud_begin(void);
void bizud_end(void);
