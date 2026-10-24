#pragma once
#include <m5os/display.h>
#include <string.h>
/* Pure rotated-framebuffer blit, shared with host bounds/scaling regression. */
static inline bool tab5_bitmap_resize(uint16_t *dst_fb,int x,int y,int w,int h,
                                     const uint16_t *px,int dw,int dh,
                                     int *first_column,int *last_column)
{
    if (!dst_fb || !px || w<=0 || h<=0 || w>4096 || h>4096 ||
        dw<=0 || dh<=0 || dw>8192 || dh>8192) return false;
    if (x>=DISPLAY_WIDTH || y>=DISPLAY_HEIGHT || x<=-dw || y<=-dh) return false;
    int x0=x<0?-x:0, y0=y<0?-y:0;
    int x1=x+dw>DISPLAY_WIDTH?DISPLAY_WIDTH-x:dw;
    int y1=y+dh>DISPLAY_HEIGHT?DISPLAY_HEIGHT-y:dh;
    for (int i=x0;i<x1;i++) {
        uint16_t *dst=&dst_fb[(size_t)(DISPLAY_WIDTH-1-(x+i))*DISPLAY_HEIGHT+y+y0];
        const uint16_t *src=px+(i*w/dw);
        for (int j=0;j<y1-y0;j++) dst[j]=src[(size_t)((j+y0)*h/dh)*w];
    }
    *first_column=x+x0; *last_column=x+x1-1;
    return true;
}

static inline bool tab5_indexed_resize_rows(uint16_t *dst_fb,int x,int y,int w,int h,
    const uint8_t *indices,const uint16_t palette[256],int dw,int dh,int *first,int *last,int clip_rows)
{
    if (!dst_fb || !indices || !palette || w<=0 || h<=0 || w>4096 || h>4096 ||
        dw<=0 || dh<=0 || dw>8192 || dh>8192) return false;
    if(clip_rows<=0) return false;
    if(clip_rows>dh) clip_rows=dh;
    if (x>=DISPLAY_WIDTH || y>=DISPLAY_HEIGHT || x<=-dw || y<=-clip_rows) return false;
    int x0=x<0?-x:0, y0=y<0?-y:0;
    int x1=x+dw>DISPLAY_WIDTH?DISPLAY_WIDTH-x:dw;
    int y1=y+clip_rows>DISPLAY_HEIGHT?DISPLAY_HEIGHT-y:clip_rows;
    for (int i=x0;i<x1;i++) {
        uint16_t *dst=&dst_fb[(size_t)(DISPLAY_WIDTH-1-(x+i))*DISPLAY_HEIGHT+y+y0];
        int sx=i*w/dw;
        for (int j=y0;j<y1;j++) dst[j-y0]=palette[indices[(size_t)(j*h/dh)*w+sx]];
    }
    *first=x+x0; *last=x+x1-1; return true;
}

static inline bool tab5_indexed_resize(uint16_t *dst_fb,int x,int y,int w,int h,
    const uint8_t *indices,const uint16_t palette[256],int dw,int dh,int *first,int *last)
{ return tab5_indexed_resize_rows(dst_fb,x,y,w,h,indices,palette,dw,dh,first,last,dh); }
