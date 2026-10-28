#pragma once
#include <m5os/types.h>

/*
 * 16x16 Japanese / Unicode BMP bitmap font (efont Biwidth 16, 10,839 glyphs:
 * ASCII, Hiragana, Katakana, Halfwidth Katakana, Fullwidth Latin/Symbols,
 * Box Drawing, Greek, Cyrillic, and JIS X 0208 Level 1 & Level 2 Kanji).
 *
 * Returns true if `cp` exists in the font and fills `rows[0..15]`
 * (where bit 15 `0x8000` is the leftmost pixel x=0, bit 0 `0x0001` is x=15).
 */
bool font_ja16_get_glyph(uint16_t cp, uint16_t rows[16], int *out_advance);
