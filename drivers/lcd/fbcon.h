#pragma once
#include <m5os/types.h>
/* Text console on the LCD: shows everything the console prints (the shell, kernel messages),
 * scrolling, with a blinking cursor.  For boards without a touch screen, where the GUI cannot run. */
void fbcon_start(void);

/* Full-screen programs (the filer) own the display while they run: pause stops fbcon from drawing,
 * resume repaints the whole console.  Console output keeps being collected while paused. */
void fbcon_pause(bool pause);
bool fbcon_is_paused(void);

/* Overlay: text drawn on top of the console without touching what the console holds, for the Japanese input
 * method (the text being composed and its candidate list).  The overlay is replaced as a whole:
 *   fbcon_overlay_begin(); fbcon_overlay_text(...) ...; fbcon_overlay_end();
 * An ANCHOR_CURSOR span is placed relative to the text cursor and follows it; an ANCHOR_SCREEN span is
 * absolute (negative row/col count from the bottom/right edge, -1 = last).  The text cursor is hidden
 * while an overlay covers it.  Without an LCD console (or on boards that have none) these do nothing. */
#define FBCON_ANCHOR_CURSOR 0
#define FBCON_ANCHOR_SCREEN 1
enum { FBCON_OVL_PREEDIT, FBCON_OVL_SEGMENT, FBCON_OVL_FOCUS, FBCON_OVL_CAND, FBCON_OVL_CAND_SEL, FBCON_OVL_BADGE, FBCON_OVL_PENDING, FBCON_OVL_BAR };
void fbcon_overlay_colors(int style, uint16_t *fg, uint16_t *bg);     /* the colours of a style, for callers that draw by themselves */
void fbcon_overlay_begin(void);
void fbcon_overlay_text(int anchor, int row, int col, const uint16_t *cps, int n, int style);
void fbcon_overlay_end(void);
void fbcon_get_geometry(int *rows, int *cols, int *cur_row, int *cur_col);

/* The console normally uses the whole screen.  fbcon_set_rows() limits it to the top ROWS text rows (at least 4,
 * at most fbcon_max_rows()); the screen below belongs to the caller (the Tab5 on-screen keyboard).  Shrinking scrolls
 * so the cursor line stays visible; growing clears the new rows.  Cells are fbcon_cell_px() pixels square. */
void fbcon_set_rows(int rows);
int  fbcon_max_rows(void);
int  fbcon_cell_px(void);
