#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include "bizud.h"
#include <m5os/display.h>
#include <m5os/kmalloc.h>
typedef struct { uint32_t cp, offset; uint16_t length; int8_t x,y; uint8_t w,h,advance; } biz_glyph;
#include "bizud24_data.inc"
static uint8_t *scratch;
bool bizud_begin(void) { if(!scratch)scratch=kmalloc_psram(10240);return scratch!=0; }
void bizud_end(void) { kfree(scratch);scratch=0; }
static const biz_glyph *find(uint32_t cp) {
    unsigned lo=0, hi=sizeof glyphs/sizeof glyphs[0];
    while(lo<hi) { unsigned m=(lo+hi)/2; if(glyphs[m].cp<cp) lo=m+1; else hi=m; }
    return lo<sizeof glyphs/sizeof glyphs[0] && glyphs[lo].cp==cp ? &glyphs[lo] : 0;
}
int bizud_width(uint32_t cp) { const biz_glyph *g=find(cp); return g?g->advance:24; }
static uint16_t blend(uint16_t f,uint16_t b,unsigned a) {
    unsigned r=(((f>>11)*a+(b>>11)*(15-a)+7)/15);
    unsigned g=((((f>>5)&63)*a+((b>>5)&63)*(15-a)+7)/15);
    unsigned bl=(((f&31)*a+(b&31)*(15-a)+7)/15);
    return (uint16_t)((r<<11)|(g<<5)|bl);
}
int bizud_draw(int x,int baseline,uint32_t cp,uint16_t fg,uint16_t bg) {
    const biz_glyph *g=find(cp);
    if(!g) { display_draw_rect(x+3,baseline-21,20,22,fg); return 24; }
    if(!scratch)return g->advance;
    uint16_t *pixels=(uint16_t *)scratch;
    if(g->w>64 || g->h>64) return g->advance;
    unsigned pos=0, total=(unsigned)g->w*g->h;
    uint8_t *packed=scratch+8192;
    unsigned bytes=(total+1)/2, n=0, src=0, length=g->length&32767;
    const uint8_t *in=coverage+g->offset;
    if(!(g->length&32768)) {
        if(length!=bytes)return g->advance;
        for(;n<bytes;n++)packed[n]=in[n];
    } else while(src<length && n<bytes) {
        unsigned flags=in[src++];
        for(unsigned bit=0;bit<8 && n<bytes;bit++) {
            if(src>=length)return g->advance;
            if(flags&(1u<<bit)) {
                if(src+1>=length)return g->advance;
                unsigned token=((unsigned)in[src]<<8)|in[src+1];src+=2;
                unsigned back=token>>4,run=(token&15)+3;
                if(!back || back>n || n+run>bytes)return g->advance;
                while(run--) {packed[n]=packed[n-back];n++;}
            } else packed[n++]=in[src++];
        }
    }
    if(n!=bytes)return g->advance;
    while(pos<total) {unsigned b=packed[pos/2];pixels[pos]=blend(fg,bg,(pos&1)?b&15:b>>4);pos++;}
    if(pos==total && total) display_draw_bitmap(x+g->x,baseline+g->y,g->w,g->h,pixels);
    return g->advance;
}
uint32_t bizud_decode(const char **text) {
    const unsigned char *p=(const unsigned char *)*text;
    uint32_t cp=*p++; unsigned n=cp>=0xf0?3:cp>=0xe0?2:cp>=0xc2?1:0;
    if(n) { cp&=(1u<<(6-n))-1; for(unsigned i=0;i<n;i++) {
        if((*p&0xc0)!=0x80) { *text=(const char *)p; return 0xfffd; }
        cp=(cp<<6)|(*p++&63);
    }}
    *text=(const char *)p; return cp;
}
int bizud_text(int x,int baseline,const char *text,int right,uint16_t fg,uint16_t bg) {
    while(*text) { uint32_t cp=bizud_decode(&text); if(x+bizud_width(cp)>right) break;
        x+=bizud_draw(x,baseline,cp,fg,bg); }
    return x;
}
#endif
