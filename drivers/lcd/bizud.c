#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include "bizud.h"
#include <m5os/display.h>
#include <m5os/kmalloc.h>
#ifndef M5OS_BIZUD_EMBEDDED
#include <m5os/vfs.h>
#include <m5os/crc32.h>
#include <m5os/hal_time.h>
#include <m5os/kprintf.h>
#include <string.h>
#endif
typedef struct { uint32_t cp, offset; uint16_t length; int8_t x,y; uint8_t w,h,advance; } biz_glyph;
#ifdef M5OS_BIZUD_EMBEDDED
#include "bizud24_data.inc"
#define glyph_count ((unsigned)(sizeof glyphs/sizeof glyphs[0]))
#else
#define BIZUD_PATH "/assets/fonts/bizud24.m5f"
#define BIZUD_HEADER 32u
static biz_glyph *glyphs;
static uint8_t *coverage;
static unsigned glyph_count;
static void *atlas;
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static bool read_all(int fd,void *out,size_t size,uint32_t *crc) {
    uint8_t *p=out;
    while(size) {
        size_t chunk=size>4096?4096:size;
        int n=vfs_read(fd,p,chunk);
        if(n!=(int)chunk)return false;
        *crc=crc32_update(*crc,p,chunk);p+=chunk;size-=chunk;
    }
    return true;
}
static bool load_atlas(void) {
    uint64_t started=hal_time_now_us();
    uint8_t h[BIZUD_HEADER]={0};uint32_t ignored=0,index_crc=0,data_crc=0;
    int fd=vfs_open(BIZUD_PATH,VFS_O_READ);if(fd<0)return false;
    bool ok=read_all(fd,h,sizeof h,&ignored);
    uint32_t count=get32(h+12),index_bytes=get32(h+16),data_bytes=get32(h+20);
    if(!ok||memcmp(h,"M5BIZ24\0",8)||get32(h+8)!=1||sizeof(biz_glyph)!=16||
       !count||count>65535||index_bytes!=count*sizeof(biz_glyph)||data_bytes>8u*1024u*1024u)goto fail;
    vfs_info_t st;if(vfs_stat(BIZUD_PATH,&st)<0||st.size!=BIZUD_HEADER+index_bytes+data_bytes)goto fail;
    atlas=kmalloc_psram(index_bytes+data_bytes);if(!atlas)goto fail;
    if(!read_all(fd,atlas,index_bytes,&index_crc)||index_crc!=get32(h+24)||
       !read_all(fd,(uint8_t *)atlas+index_bytes,data_bytes,&data_crc)||data_crc!=get32(h+28))goto fail;
    glyphs=atlas;coverage=(uint8_t *)atlas+index_bytes;glyph_count=count;
    for(unsigned i=0;i<count;i++) {
        biz_glyph *g=&glyphs[i];uint32_t length=g->length&32767u;
        if((i&&glyphs[i-1].cp>=g->cp)||g->offset>data_bytes||length>data_bytes-g->offset)goto fail;
    }
    vfs_close(fd);
    kprintf("bizud: loaded %u glyphs, %u bytes from SD in %u ms\n",count,index_bytes+data_bytes,
            (uint32_t)((hal_time_now_us()-started)/1000u));
    return true;
fail:
    vfs_close(fd);kfree(atlas);atlas=0;glyphs=0;coverage=0;glyph_count=0;return false;
}
#endif
static uint8_t *scratch;
bool bizud_begin(void) {
#ifndef M5OS_BIZUD_EMBEDDED
    if(!atlas&&!load_atlas())return false;
#endif
    if(!scratch)scratch=kmalloc_psram(10240);
    if(scratch)return true;
#ifndef M5OS_BIZUD_EMBEDDED
    kfree(atlas);atlas=0;glyphs=0;coverage=0;glyph_count=0;
#endif
    return false;
}
void bizud_end(void) {
    kfree(scratch);scratch=0;
#ifndef M5OS_BIZUD_EMBEDDED
    kfree(atlas);atlas=0;glyphs=0;coverage=0;glyph_count=0;
#endif
}
static const biz_glyph *find(uint32_t cp) {
    unsigned lo=0, hi=glyph_count;
    while(lo<hi) { unsigned m=(lo+hi)/2; if(glyphs[m].cp<cp) lo=m+1; else hi=m; }
    return lo<glyph_count && glyphs[lo].cp==cp ? &glyphs[lo] : 0;
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
