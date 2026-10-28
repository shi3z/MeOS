#pragma once
#include <m5os/types.h>

/*
 * Dragon-Quest-style text windows: black box, thick white frame with rounded corners, white text,
 * a triangular cursor in front of the chosen line.  Everything is placed on a grid of character
 * cells (a cell is one 8x8 font glyph, scaled by M5OS_FBCON_SCALE), so the same code works on a
 * 40-column LCD and on the Tab5's 80-column screen.  Drawing goes through the display API
 * (m5os/display.h); input comes from the console, so it works with a keyboard or a serial terminal.
 */
int  dq_cols(void);                                   /* screen size in cells */
int  dq_rows(void);

void dq_clear(void);                                  /* black screen */
void dq_window(int cx, int cy, int cw, int ch);       /* frame + black inside; size in cells, frame is drawn inside the box */
void dq_text(int cx, int cy, const char *s);          /* white on black, clipped to the screen */
void dq_text_rev(int cx, int cy, const char *s);      /* inverse (black on white): the highlighted line */
void dq_cursor(int cx, int cy, bool on);              /* the triangle, one cell wide, in front of a line */

/* Keys.  Printable characters are returned as themselves; the rest are the DQK_* codes. */
enum { DQK_NONE = -1, DQK_ESC = 0x1B, DQK_ENTER = '\r', DQK_BACKSPACE = 0x7F,
       DQK_UP = 0x100, DQK_DOWN, DQK_LEFT, DQK_RIGHT, DQK_PGUP, DQK_PGDN, DQK_HOME, DQK_END, DQK_DEL,
       DQK_F1, DQK_F2, DQK_F3, DQK_F4,
       DQK_REFRESH };      /* not a key: the screen was disturbed (the on-screen keyboard covered part of it) - repaint */
int  dq_getkey(void);                                 /* blocks (sleeping) until a key arrives */
int  dq_pollkey(void);                                /* non-blocking: returns DQK_NONE if no key is queued */

/* Ready-made dialogs; each draws its own window in the middle of the screen and leaves it there
 * (the caller repaints what is underneath). */
bool dq_confirm(const char *question);                /* "Yes / No" menu; ESC = No */
bool dq_input(const char *title, char *buf, int max); /* one-line text field; false on ESC */
void dq_message(const char *line1, const char *line2);/* waits for a key */
