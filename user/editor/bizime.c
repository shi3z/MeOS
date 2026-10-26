#include "bizime.h"
#include "../../drivers/lcd/bizud.h"
#include "../../drivers/lcd/otfont.h"
#include "../../drivers/lcd/fbcon.h"
#include <m5os/display.h>
#include <m5os/kprintf.h>

#define POP_GAP 8
#define POP_PAD 10
#define POP_COLS 2

static bool use_outline;
static unsigned font_pixels=24;
void bizime_set_font(bool outline,unsigned pixel_size) {
    use_outline=outline;font_pixels=pixel_size?pixel_size:24;
}
static int glyph_top(void) {return (int)font_pixels+3;}
static int glyph_box(void) {return (int)font_pixels+8;}
static int popup_row(void) {return (int)font_pixels+10;}
static int font_width(uint32_t cp) {return use_outline?otfont_width(cp):bizud_width(cp);}
static int font_draw(int x,int baseline,uint32_t cp,uint16_t fg,uint16_t bg) {
    return use_outline?otfont_draw(x,baseline,cp,fg,bg):bizud_draw(x,baseline,cp,fg,bg);
}
static int font_vertical_advance(uint32_t cp) {
    return use_outline?otfont_vertical_advance(cp):bizud_vertical_advance(cp);
}
static int font_draw_vertical(int x,int top,uint32_t cp,uint16_t fg,uint16_t bg) {
    return use_outline?otfont_draw_vertical(x,top,cp,fg,bg):bizud_draw_vertical(x,top,cp,fg,bg);
}
static int font_text(int x,int baseline,const char *text,int right,uint16_t fg,uint16_t bg) {
    if(!use_outline)return bizud_text(x,baseline,text,right,fg,bg);
    return otfont_text(x,baseline,text,right,fg,bg);
}

static void draw_candidate(const ime_view *view, int index, int x, int baseline,
                           int width, uint16_t fg, uint16_t bg, uint16_t accent)
{
    bool selected = index == view->selected;
    uint16_t item_fg = selected ? bg : fg;
    uint16_t item_bg = selected ? accent : bg;
    display_fill_rect(x, baseline-glyph_top(), width, popup_row(), item_bg);
    char number[8];
    ksnprintf(number, sizeof number, "%d", view->numbers[index]);
    int tx = font_text(x + 8, baseline, number, x + width - 8, item_fg, item_bg);
    tx = font_text(tx + 5, baseline, view->candidates[index], x + width - 8, item_fg, item_bg);
    (void)tx;
}

static void draw_popup(const ime_view *view, int anchor_x, int baseline,
                       int left, int right, int top, int bottom,
                       uint16_t fg, uint16_t bg, uint16_t accent)
{
    if (!view->candidate_count) return;
    int rows = (view->candidate_count + POP_COLS - 1) / POP_COLS;
    int available = right - left;
    int width = available * 3 / 4;
    if (width > 900) width = 900;
    if (width < 520) width = available;
    int height = rows*popup_row()+POP_PAD*2;
    int x = anchor_x;
    if (x + width > right) x = right - width;
    if (x < left) x = left;
    int y = baseline + POP_GAP;
    if (y + height > bottom) y = baseline-glyph_top()-POP_GAP-height;
    if (y < top) y = top;

    display_fill_rect(x, y, width, height, bg);
    display_draw_rect(x, y, width, height, accent);
    int col_width = (width - POP_PAD * 2) / POP_COLS;
    for (int n = 0; n < view->candidate_count; n++) {
        int col = n / rows, row = n % rows;
        int item_x = x + POP_PAD + col * col_width;
        int item_y = y+POP_PAD+row*popup_row()+glyph_top();
        draw_candidate(view, n, item_x, item_y, col_width - 4, fg, bg, accent);
    }
}

static void draw_vertical_popup(const ime_view *view, int anchor_x, int anchor_y,
                                int left, int right, int top, int bottom,
                                uint16_t fg, uint16_t bg, uint16_t accent)
{
    if (!view->candidate_count) return;
    int rows = (view->candidate_count + POP_COLS - 1) / POP_COLS;
    int available = right - left;
    int width = available * 3 / 4;
    if (width > 720) width = 720;
    if (width < 520) width = available;
    int height = rows*popup_row()+POP_PAD*2;
    int x = anchor_x - POP_GAP - width;
    if (x < left) x = left;
    if (x + width > right) x = right - width;
    int y = anchor_y;
    if (y + height > bottom) y = bottom - height;
    if (y < top) y = top;
    display_fill_rect(x, y, width, height, bg);
    display_draw_rect(x, y, width, height, accent);
    int col_width = (width - POP_PAD * 2) / POP_COLS;
    for (int n = 0; n < view->candidate_count; n++) {
        int col = n / rows, row = n % rows;
        int item_x = x + POP_PAD + col * col_width;
        int item_y = y+POP_PAD+row*popup_row()+glyph_top();
        draw_candidate(view, n, item_x, item_y, col_width - 4, fg, bg, accent);
    }
}

bizime_point bizime_measure(const ime_view *view, int x, int baseline,
                            int left, int right, int pitch)
{
    bizime_point end = {x, baseline};
    if (!view) return end;
    for (int n = 0; n < view->count; n++) {
        int advance = font_width(view->text[n]);
        if (end.x + advance > right) {
            end.x = left;
            end.baseline += pitch;
        }
        end.x += advance;
    }
    return end;
}

bizime_point bizime_draw(const ime_view *view, int x, int baseline,
                         int left, int right, int top, int bottom, int pitch,
                         uint16_t fg, uint16_t bg, uint16_t accent)
{
    bizime_point end = {x, baseline};
    if (!view || !view->count) return end;
    int anchor_x = x;
    for (int n = 0; n < view->count; n++) {
        int advance = font_width(view->text[n]);
        if (end.x + advance > right) {
            end.x = left;
            end.baseline += pitch;
        }
        if (end.baseline > bottom) break;
        uint16_t glyph_fg = fg, glyph_bg = bg;
        if (view->styles[n] == FBCON_OVL_FOCUS) {
            glyph_fg = bg;
            glyph_bg = accent;
        } else if (view->styles[n] == FBCON_OVL_PENDING) {
            glyph_fg = accent;
        }
        display_fill_rect(end.x,end.baseline-glyph_top(),advance,glyph_box(),glyph_bg);
        font_draw(end.x, end.baseline, view->text[n], glyph_fg, glyph_bg);
        if (view->styles[n] != FBCON_OVL_FOCUS)
            display_fill_rect(end.x, end.baseline + 3, advance, 2, accent);
        end.x += advance;
    }
    display_fill_rect(end.x,end.baseline-glyph_top(),2,glyph_box(),accent);
    draw_popup(view, anchor_x, end.baseline, left, right, top, bottom, fg, bg, accent);
    return end;
}

bizime_vpoint bizime_measure_vertical(const ime_view *view, int x, int y,
                                      int top, int bottom, int column_pitch,
                                      int tracking)
{
    bizime_vpoint end = {x, y};
    if (!view) return end;
    for (int n = 0; n < view->count; n++) {
        int step = font_vertical_advance(view->text[n]) + tracking;
        if (end.y + step > bottom) {
            end.y = top;
            end.x -= column_pitch;
        }
        end.y += step;
    }
    return end;
}

bizime_vpoint bizime_draw_vertical(const ime_view *view, int x, int y,
                                   int left, int right, int top, int bottom,
                                   int column_pitch, int tracking,
                                   uint16_t fg, uint16_t bg, uint16_t accent)
{
    bizime_vpoint end = {x, y};
    if (!view || !view->count) return end;
    int anchor_x = x, anchor_y = y;
    for (int n = 0; n < view->count; n++) {
        int advance = font_vertical_advance(view->text[n]);
        int step = advance + tracking;
        if (end.y + step > bottom) {
            end.y = top;
            end.x -= column_pitch;
        }
        if (end.x < left) break;
        uint16_t glyph_fg = fg, glyph_bg = bg;
        if (view->styles[n] == FBCON_OVL_FOCUS) {
            glyph_fg = bg;
            glyph_bg = accent;
        } else if (view->styles[n] == FBCON_OVL_PENDING) {
            glyph_fg = accent;
        }
        display_fill_rect(end.x,end.y,(int)font_pixels,step,glyph_bg);
        font_draw_vertical(end.x, end.y, view->text[n], glyph_fg, glyph_bg);
        if (view->styles[n] != FBCON_OVL_FOCUS)
            display_fill_rect(end.x+(int)font_pixels-2,end.y,2,advance,accent);
        end.y += step;
    }
    display_fill_rect(end.x,end.y,(int)font_pixels,2,accent);
    draw_vertical_popup(view, anchor_x, anchor_y, left, right, top, bottom,
                        fg, bg, accent);
    return end;
}
