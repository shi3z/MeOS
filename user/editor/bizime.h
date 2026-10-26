#pragma once
#include <m5os/ime.h>
#include <m5os/types.h>

typedef struct {
    int x;
    int baseline;
} bizime_point;

typedef struct {
    int x;
    int y;
} bizime_vpoint;

/* Select the already-open outline renderer for the active editor.  False is
 * the fixed BIZ UD atlas used by Scheme and by the Write fallback path. */
void bizime_set_font(bool outline, unsigned pixel_size);

/* Return the insertion point after the preedit, including proportional wrap,
 * without drawing. Editors use this to reflow committed text behind it. */
bizime_point bizime_measure(const ime_view *view, int x, int baseline,
                            int left, int right, int pitch);

/* Draw a proportional BIZ UD preedit at the document insertion point and a
 * bounded two-column candidate popup close to it. The caller owns the
 * framebuffer and repaints the underlying document on every revision. */
bizime_point bizime_draw(const ime_view *view, int x, int baseline,
                         int left, int right, int top, int bottom, int pitch,
                         uint16_t fg, uint16_t bg, uint16_t accent);

/* Vertical prose flows down, then into the column at x-column_pitch.  Tracking
 * is added to the font's vertical advance so the editor and IME reflow agree. */
bizime_vpoint bizime_measure_vertical(const ime_view *view, int x, int y,
                                      int top, int bottom, int column_pitch,
                                      int tracking);
bizime_vpoint bizime_draw_vertical(const ime_view *view, int x, int y,
                                   int left, int right, int top, int bottom,
                                   int column_pitch, int tracking,
                                   uint16_t fg, uint16_t bg, uint16_t accent);
