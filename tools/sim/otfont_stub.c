#include "../../drivers/lcd/otfont.h"
#include <string.h>

bool otfont_begin(const char *path,unsigned pixel_size) {(void)path;(void)pixel_size;return false;}
bool otfont_begin_resident(const char *path,unsigned pixel_size) {(void)path;(void)pixel_size;return false;}
bool otfont_set_pixel_size(unsigned pixel_size) {(void)pixel_size;return false;}
void otfont_end(void) {}
int otfont_width(uint32_t cp) {(void)cp;return 0;}
int otfont_draw(int x,int baseline,uint32_t cp,uint16_t fg,uint16_t bg) {
    (void)x;(void)baseline;(void)cp;(void)fg;(void)bg;return 0;
}
int otfont_vertical_advance(uint32_t cp) {(void)cp;return 0;}
int otfont_draw_vertical(int x,int top,uint32_t cp,uint16_t fg,uint16_t bg) {
    (void)x;(void)top;(void)cp;(void)fg;(void)bg;return 0;
}
int otfont_text(int x,int baseline,const char *text,int right,uint16_t fg,uint16_t bg) {
    (void)baseline;(void)text;(void)right;(void)fg;(void)bg;return x;
}
void otfont_get_stats(otfont_stats *out) {if(out)memset(out,0,sizeof *out);}
