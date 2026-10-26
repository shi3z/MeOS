#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../drivers/lcd/bitmap_resize.h"
static uint16_t source[320*200], palette[256];
static uint8_t indices[320*200];
static void check(int x,int y,int w,int h,int dw,int dh)
{
    size_t n=DISPLAY_WIDTH*DISPLAY_HEIGHT;
    uint16_t *allocation=malloc((n+4)*2);assert(allocation);
    for(size_t i=0;i<n+4;i++) allocation[i]=0xbeef;
    uint16_t *out=allocation+2;
    int first=-1,last=-1;
    bool ok=tab5_bitmap_resize(out,x,y,w,h,source,dw,dh,&first,&last);
    bool visible=false;
    for(int dx=0;dx<DISPLAY_WIDTH;dx++) for(int dy=0;dy<DISPLAY_HEIGHT;dy++) {
        uint16_t want=0xbeef;
        if(dx>=x && dx-x<dw && dy>=y && dy-y<dh) {
            want=source[(size_t)((dy-y)*h/dh)*w+(dx-x)*w/dw];visible=true;
            assert(first<=dx && last>=dx);
        }
        assert(out[(size_t)(DISPLAY_WIDTH-1-dx)*DISPLAY_HEIGHT+dy]==want);
    }
    assert(ok==visible && allocation[0]==0xbeef && allocation[n+2]==0xbeef && allocation[n+3]==0xbeef && allocation[1]==0xbeef);
    for(size_t i=0;i<n+4;i++) allocation[i]=0xbeef;
    ok=tab5_indexed_resize(out,x,y,w,h,indices,palette,dw,dh,&first,&last);
    assert(ok==visible);
    for(int dx=0;dx<DISPLAY_WIDTH;dx++) for(int dy=0;dy<DISPLAY_HEIGHT;dy++) {
        uint16_t want=0xbeef;
        if(dx>=x && dx-x<dw && dy>=y && dy-y<dh)
            want=source[(size_t)((dy-y)*h/dh)*w+(dx-x)*w/dw];
        assert(out[(size_t)(DISPLAY_WIDTH-1-dx)*DISPLAY_HEIGHT+dy]==want);
    }
    assert(allocation[0]==0xbeef && allocation[n+2]==0xbeef && allocation[n+3]==0xbeef && allocation[1]==0xbeef);
    free(allocation);
}
int main(void)
{
    for(unsigned i=0;i<256;i++) palette[i]=i*251;
    for(unsigned i=0;i<320*200;i++) { indices[i]=i%256; source[i]=palette[indices[i]]; }
    check(160,0,320,200,960,720); /* complete image and untouched side rails */
    check(-3,-5,3,2,9,8);check(1276,717,3,2,9,8);
    check(0,0,3,2,1,1);check(1280,720,3,2,9,8);
    check(-9,-8,3,2,9,8);
    uint16_t pixel=123;int first,last;
    assert(!tab5_bitmap_resize(&pixel,0,0,3,2,source,0,720,&first,&last));
    assert(!tab5_bitmap_resize(&pixel,0,0,3,2,source,960,8193,&first,&last));
    assert(!tab5_bitmap_resize(&pixel,INT32_MIN,INT32_MIN,3,2,source,9,8,&first,&last));
    assert(pixel==123);
    /* Prefix rendering preserves an existing HUD without rescaling the world. */
    uint16_t *full=calloc(DISPLAY_WIDTH*DISPLAY_HEIGHT,2), *partial=calloc(DISPLAY_WIDTH*DISPLAY_HEIGHT,2);
    assert(full && partial);
    assert(tab5_indexed_resize(full,160,0,320,200,indices,palette,960,720,&first,&last));
    memcpy(partial,full,DISPLAY_WIDTH*DISPLAY_HEIGHT*2);
    for(unsigned i=0;i<320*168;i++) indices[i]^=127;
    assert(tab5_indexed_resize_rows(partial,160,0,320,200,indices,palette,960,720,&first,&last,605));
    assert(tab5_indexed_resize(full,160,0,320,200,indices,palette,960,720,&first,&last));
    assert(!memcmp(full,partial,DISPLAY_WIDTH*DISPLAY_HEIGHT*2));
    free(full);free(partial);
    puts("Tab5 resize: full-height 4:3 image, clipping, untouched rails and bounds PASS");
}
