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
    uint32_t rows[DISPLAY_HEIGHT];
    for (int j=y0;j<y1;j++) rows[j-y0]=(uint32_t)(j*h/dh)*w;
    for (int i=x0;i<x1;i++) {
        uint16_t *dst=&dst_fb[(size_t)(DISPLAY_WIDTH-1-(x+i))*DISPLAY_HEIGHT+y+y0];
        const uint16_t *src=px+(i*w/dw);
        for (int j=0;j<y1-y0;j++) dst[j]=src[rows[j]];
    }
    *first_column=x+x0; *last_column=x+x1-1;
    return true;
}

/* Use alias-safe word loads/stores. Congruent halfword offsets peel one pixel;
 * incompatible alignments retain the generic memcpy fallback. */
static inline void tab5_copy_column(uint16_t *dst,const uint16_t *src,unsigned pixels)
{
    if (!pixels) return;
    if (((uintptr_t)dst | (uintptr_t)src) & 3u) {
        if ((((uintptr_t)dst ^ (uintptr_t)src) & 3u) || ((uintptr_t)dst & 1u)) {
            memcpy(dst,src,(size_t)pixels*2); return;
        }
        /* Rotated framebuffer rows and the SRAM column are both word-aligned.
         * Odd damage starts offset both by one halfword. Peel that pixel rather
         * than falling back to ROM memcpy for the entire column. */
        __builtin_memcpy(dst,src,sizeof(uint16_t));
        dst++;src++;pixels--;
    }
    typedef uint32_t alias_word __attribute__((may_alias));
    alias_word *d=(alias_word *)dst;
    const alias_word *s=(const alias_word *)src;
    unsigned n=pixels/2;
    while(n>=8) {
        d[0]=s[0];d[1]=s[1];d[2]=s[2];d[3]=s[3];
        d[4]=s[4];d[5]=s[5];d[6]=s[6];d[7]=s[7];
        d+=8;s+=8;n-=8;
    }
    while(n--) *d++=*s++;
    if(pixels&1u) __builtin_memcpy(dst+pixels-1,(const unsigned char *)src+(pixels-1)*sizeof(uint16_t),sizeof(uint16_t));
}
/* Three physical rows contain the same source column. Load each SRAM word once
 * and broadcast it to the three destinations instead of rereading it thrice. */
static inline void tab5_copy_column_triplet(uint16_t *dst,const uint16_t *src,unsigned pixels)
{
    if(!pixels)return;
    uint16_t *b=dst+DISPLAY_HEIGHT,*c=b+DISPLAY_HEIGHT;
    if((((uintptr_t)dst^(uintptr_t)src)&3u)||((uintptr_t)dst&1u)){
        tab5_copy_column(dst,src,pixels);tab5_copy_column(b,src,pixels);tab5_copy_column(c,src,pixels);return;
    }
    if((uintptr_t)dst&2u){
        __builtin_memcpy(dst,src,2);__builtin_memcpy(b,src,2);__builtin_memcpy(c,src,2);
        dst++;b++;c++;src++;pixels--;
    }
    typedef uint32_t alias_word __attribute__((may_alias));
    alias_word *d0=(alias_word *)dst,*d1=(alias_word *)b,*d2=(alias_word *)c;
    const alias_word *s=(const alias_word *)src;unsigned n=pixels/2;
    while(n>=4){
        for(unsigned i=0;i<4;i++){uint32_t v=s[i];d0[i]=v;d1[i]=v;d2[i]=v;}
        s+=4;d0+=4;d1+=4;d2+=4;n-=4;
    }
    while(n--){uint32_t v=*s++;*d0++=v;*d1++=v;*d2++=v;}
    if(pixels&1u){
        const uint16_t *tail=src+pixels-1;
        __builtin_memcpy(dst+pixels-1,tail,2);__builtin_memcpy(b+pixels-1,tail,2);__builtin_memcpy(c+pixels-1,tail,2);
    }
}
/* Cache-friendly 320x200 -> 960x720 path: read eight adjacent source
 * columns together, then expand from a small SRAM tile. This avoids repeatedly
 * fetching the same external-RAM cache lines through a strided source walk. */
static inline void tab5_indexed_viewport_spans(uint16_t *fb,int x,const uint8_t *indices,
                                       const uint16_t palette[256],unsigned output_rows,
                                       const uint8_t *first,const uint8_t *last)
{
    uint16_t tile[8][200];
    uint32_t column[360] __attribute__((aligned(16)));
    for(unsigned bx=0;bx<320;bx+=8) {
        if(first){bool any=false;for(unsigned c=0;c<8;c++)if(first[bx+c]<200)any=true;if(!any)continue;}
        for(unsigned sy=0;sy<200;sy++) {
            const uint8_t *in=indices+sy*320+bx;
            for(unsigned c=0;c<8;c++) tile[c][sy]=palette[in[c]];
        }
        for(unsigned c=0;c<8;c++) {
            if(first&&first[bx+c]>=200)continue;
            unsigned start=first?((unsigned)first[bx+c]*18+4)/5:0;
            unsigned end=last?(((unsigned)last[bx+c]+1)*18+4)/5:output_rows;
            if(end>output_rows)end=output_rows;
            uint32_t *out=column;
            for(unsigned sy=0;sy<200;sy+=5) {
                uint32_t a=tile[c][sy],b=tile[c][sy+1],v=tile[c][sy+2],d=tile[c][sy+3],e=tile[c][sy+4];
                /* Five input rows become 4+4+3+4+3 output rows, exactly
                 * matching floor(dest_y*200/720), packed in little endian. */
                out[0]=out[1]=a|(a<<16); out[2]=out[3]=b|(b<<16);
                out[4]=v|(v<<16); out[5]=v|(d<<16); out[6]=d|(d<<16);
                out[7]=d|(e<<16); out[8]=e|(e<<16);out+=9;
            }
            unsigned physical=DISPLAY_WIDTH-3-(unsigned)x-(bx+c)*3;
            tab5_copy_column_triplet(fb+physical*DISPLAY_HEIGHT+start,(const uint16_t *)column+start,end-start);
        }
    }
}
static inline void tab5_indexed_viewport(uint16_t *fb,int x,const uint8_t *indices,
                                       const uint16_t palette[256],unsigned output_rows)
{ tab5_indexed_viewport_spans(fb,x,indices,palette,output_rows,NULL,NULL); }
/* Exact damage tracking: palette changes repaint everything; unchanged indices
 * retain the previous framebuffer pixels. Source row bounds map to the same
 * floor(dest_y*200/720) sampling as the full blitter, including partial rows. */
static inline bool tab5_indexed_damage(const uint8_t *pixels,uint8_t *previous,bool force,
    uint8_t first[320],uint8_t last[320],unsigned *left,unsigned *right)
{
    memset(first,200,320);memset(last,0,320);
    if(force){memset(first,0,320);memset(last,199,320);memcpy(previous,pixels,64000);}
    else
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (!(((uintptr_t)pixels | (uintptr_t)previous) & 3u)) {
        typedef uint32_t alias_word __attribute__((may_alias));
        const alias_word *now=(const alias_word *)pixels;
        alias_word *old=(alias_word *)previous;
        for(unsigned y=0;y<200;y++)for(unsigned col=0;col<320;col+=4){
            unsigned at=(y*320+col)/4;uint32_t value=now[at],diff=value^old[at];
            if(!diff)continue;
            old[at]=value;
            for(unsigned byte=0;byte<4;byte++,diff>>=8)if(diff&255u){
                if(first[col+byte]==200)first[col+byte]=(uint8_t)y;
                last[col+byte]=(uint8_t)y;
            }
        }
    } else
#endif
    for(unsigned y=0;y<200;y++)for(unsigned col=0;col<320;col++){
        unsigned at=y*320+col;uint8_t p=pixels[at];
        if(previous[at]!=p){if(first[col]==200)first[col]=(uint8_t)y;last[col]=(uint8_t)y;previous[at]=p;}
    }
    *left=320;*right=0;
    for(unsigned col=0;col<320;col++)if(first[col]<200){if(*left==320)*left=col;*right=col;}
    return *left!=320;
}
static inline bool tab5_indexed_viewport_delta(uint16_t *fb,int x,const uint8_t *pixels,
    uint8_t *previous,const uint16_t palette[256],bool force,int *first_col,int *last_col)
{
    if(!fb||!pixels||!previous||!palette||x<0||x>DISPLAY_WIDTH-960)return false;
    uint8_t first[320],last[320];unsigned left,right;
    if(!tab5_indexed_damage(pixels,previous,force,first,last,&left,&right))return false;
    tab5_indexed_viewport_spans(fb,x,pixels,palette,720,first,last);
    *first_col=x+(int)left*3;*last_col=x+(int)right*3+2;return true;
}
/* A source column is expanded once into SRAM, then copied to repeated physical
 * columns in bulk. In particular 320->960 does not repeat palette lookup three
 * times and does not read/write a full RGB intermediate in external RAM. */
static inline bool tab5_indexed_resize_rows(uint16_t *dst_fb,int x,int y,int w,int h,
    const uint8_t *indices,const uint16_t palette[256],int dw,int dh,int *first,int *last,int clip_rows)
{
    if (!dst_fb || !indices || !palette || w<=0 || h<=0 || w>4096 || h>4096 ||
        dw<=0 || dh<=0 || dw>8192 || dh>8192) return false;
    if(clip_rows<=0) return false;
    if(clip_rows>dh) clip_rows=dh;
    if (x>=DISPLAY_WIDTH || y>=DISPLAY_HEIGHT || x<=-dw || y<=-clip_rows) return false;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(w==320 && h==200 && dw==960 && dh==720 && y==0 && x>=0 && x<=DISPLAY_WIDTH-960) {
        tab5_indexed_viewport(dst_fb,x,indices,palette,(unsigned)clip_rows);
        *first=x;*last=x+959;return true;
    }
#endif
    int x0=x<0?-x:0, y0=y<0?-y:0;
    int x1=x+dw>DISPLAY_WIDTH?DISPLAY_WIDTH-x:dw;
    int y1=y+clip_rows>DISPLAY_HEIGHT?DISPLAY_HEIGHT-y:clip_rows;
    uint32_t rows[DISPLAY_HEIGHT];
    uint16_t column[DISPLAY_HEIGHT] __attribute__((aligned(16)));
    for (int j=y0;j<y1;j++) rows[j-y0]=(uint32_t)(j*h/dh)*w;
    int previous=-1;
    for (int i=x0;i<x1;i++) {
        int sx=i*w/dw;
        if (sx!=previous) {
            const uint8_t *src=indices+sx;
            for(int j=0;j<y1-y0;j++) column[j]=palette[src[rows[j]]];
            previous=sx;
        }
        uint16_t *dst=&dst_fb[(size_t)(DISPLAY_WIDTH-1-(x+i))*DISPLAY_HEIGHT+y+y0];
        tab5_copy_column(dst,column,(unsigned)(y1-y0));
    }
    *first=x+x0; *last=x+x1-1; return true;
}

static inline bool tab5_indexed_resize(uint16_t *dst_fb,int x,int y,int w,int h,
    const uint8_t *indices,const uint16_t palette[256],int dw,int dh,int *first,int *last)
{ return tab5_indexed_resize_rows(dst_fb,x,y,w,h,indices,palette,dw,dh,first,last,dh); }
