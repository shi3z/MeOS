#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include "otfont.h"
#include "bizud.h"
#include <m5os/display.h>
#include <m5os/errno.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/sdroot.h>
#include <m5os/vfs.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_MODULE_H
#include FT_SYSTEM_H
#include <string.h>

#define OTFONT_MAX_FILE (8u*1024u*1024u)
#define OTFONT_MAX_GLYPH 64u
#define OTFONT_CELL_BYTES ((OTFONT_MAX_GLYPH*OTFONT_MAX_GLYPH)/2u)
#define OTFONT_CACHE_SLOTS 384u
#define OTFONT_METRIC_SETS 256u
#define OTFONT_METRIC_WAYS 4u
#define OTFONT_METRIC_SLOTS (OTFONT_METRIC_SETS*OTFONT_METRIC_WAYS)
#define OTFONT_STREAM_PAGE (16u*1024u)
#define OTFONT_STREAM_SLOTS 16u
#define OTFONT_STREAM_CACHE (OTFONT_STREAM_PAGE*OTFONT_STREAM_SLOTS)

typedef struct {
    uint32_t glyph,stamp;
    uint16_t pixel_size,advance,vadvance;
    int16_t left,top;
    uint8_t width,height,valid;
} cache_entry;

typedef struct {
    uint32_t glyph,stamp;
    uint16_t advance,vadvance;
    uint8_t valid;
} metric_entry;

typedef struct {uint32_t cp;uint16_t glyph;} vertical_map_entry;
#include "otfont_bizud_vertical.inc"

typedef struct {
    struct FT_MemoryRec_ memory;
    struct FT_StreamRec_ stream;
    FT_Library library;
    FT_Face face;
    uint8_t *font_data,*stream_cache,*cells;
    uint16_t *scratch;
    cache_entry *entries;
    metric_entry *metrics;
    int font_fd;
    uint32_t font_bytes,stream_base[OTFONT_STREAM_SLOTS],stream_length[OTFONT_STREAM_SLOTS];
    uint32_t stream_stamp[OTFONT_STREAM_SLOTS],stream_clock,load_ms,stamp,hits,misses;
    uint32_t metric_stamp,metric_hits,metric_misses;
    uint32_t io_bytes,io_reads;
    uint64_t raster_us;
    unsigned pixel_size,users;
} outline_font;

static outline_font s_font;

static void *font_alloc(size_t bytes) {
    void *p=kmalloc_psram(bytes);
    if(!p&&bizud_trim())p=kmalloc_psram(bytes);
    if(!p&&sdroot_asset_cache_trim())p=kmalloc_psram(bytes);
    return p;
}
static void *ft_alloc(FT_Memory memory,long size) {
    (void)memory;return size>0?font_alloc((size_t)size):0;
}
static void ft_free(FT_Memory memory,void *block) {
    (void)memory;kfree(block);
}
static void *ft_realloc(FT_Memory memory,long current,long requested,void *block) {
    (void)memory;
    if(requested<=0){kfree(block);return 0;}
    void *next=font_alloc((size_t)requested);if(!next)return 0;
    if(block&&current>0)memcpy(next,block,(size_t)(current<requested?current:requested));
    kfree(block);return next;
}

static bool fill_stream_cache(uint32_t offset,unsigned *slot_out) {
    unsigned slot=0;uint32_t oldest=UINT32_MAX;
    for(unsigned i=0;i<OTFONT_STREAM_SLOTS;i++) {
        if(!s_font.stream_length[i]){slot=i;oldest=0;break;}
        if(s_font.stream_stamp[i]<oldest){oldest=s_font.stream_stamp[i];slot=i;}
    }
    uint32_t base=offset&~(OTFONT_STREAM_PAGE-1u);
    uint32_t length=s_font.font_bytes-base;if(length>OTFONT_STREAM_PAGE)length=OTFONT_STREAM_PAGE;
    if(vfs_seek(s_font.font_fd,(int32_t)base,FS_SEEK_SET)!=(int)base)return false;
    uint32_t done=0;
    while(done<length) {
        int n=vfs_read(s_font.font_fd,s_font.stream_cache+slot*OTFONT_STREAM_PAGE+done,length-done);
        if(n<=0)return false;
        done+=(uint32_t)n;
    }
    s_font.stream_base[slot]=base;s_font.stream_length[slot]=length;
    s_font.stream_stamp[slot]=++s_font.stream_clock;
    s_font.io_bytes+=length;s_font.io_reads++;*slot_out=slot;return true;
}

static unsigned long stream_read(FT_Stream stream,unsigned long offset,
                                 unsigned char *buffer,unsigned long count) {
    (void)stream;
    if(offset>s_font.font_bytes||count>s_font.font_bytes-offset)return 0;
    if(!count)return 0;
    unsigned long done=0;
    while(done<count) {
        uint32_t at=(uint32_t)(offset+done);
        unsigned slot=OTFONT_STREAM_SLOTS;
        for(unsigned i=0;i<OTFONT_STREAM_SLOTS;i++) {
            if(s_font.stream_length[i]&&at>=s_font.stream_base[i]&&
               at<s_font.stream_base[i]+s_font.stream_length[i]){slot=i;break;}
        }
        if(slot==OTFONT_STREAM_SLOTS&&!fill_stream_cache(at,&slot))break;
        s_font.stream_stamp[slot]=++s_font.stream_clock;
        uint32_t in_page=at-s_font.stream_base[slot];
        uint32_t available=s_font.stream_length[slot]-in_page;
        unsigned long chunk=count-done;if(chunk>available)chunk=available;
        memcpy(buffer+done,s_font.stream_cache+slot*OTFONT_STREAM_PAGE+in_page,chunk);done+=chunk;
    }
    return done;
}

static void stream_close(FT_Stream stream) {
    int fd=(int)stream->descriptor.value;
    if(fd>=0)vfs_close(fd);
    stream->descriptor.value=-1;s_font.font_fd=-1;
}

static bool open_stream(const char *path) {
    vfs_info_t st;if(vfs_stat(path,&st)<0||st.type!=FS_TYPE_FILE||!st.size||st.size>OTFONT_MAX_FILE)return false;
    int fd=vfs_open(path,VFS_O_READ);if(fd<0)return false;
    s_font.stream_cache=font_alloc(OTFONT_STREAM_CACHE);
    if(!s_font.stream_cache){vfs_close(fd);return false;}
    s_font.font_fd=fd;s_font.font_bytes=st.size;
    memset(&s_font.stream,0,sizeof s_font.stream);
    s_font.stream.size=st.size;s_font.stream.descriptor.value=fd;
    s_font.stream.read=stream_read;s_font.stream.close=stream_close;
    return true;
}

static bool open_resident(const char *path,uint32_t bytes)
{
    s_font.font_data=font_alloc(bytes);if(!s_font.font_data)return false;
    int fd=vfs_open(path,VFS_O_READ);if(fd<0)return false;
    uint32_t done=0;
    while(done<bytes) {
        int n=vfs_read(fd,s_font.font_data+done,bytes-done);s_font.io_reads++;
        if(n<=0)break;
        done+=(uint32_t)n;s_font.io_bytes+=(uint32_t)n;
    }
    int close_rc=vfs_close(fd);
    return done==bytes&&close_rc>=0;
}

static void release_all(void) {
    if(s_font.face)FT_Done_Face(s_font.face);
    else if(s_font.font_fd>=0)vfs_close(s_font.font_fd);
    if(s_font.library)FT_Done_Library(s_font.library);
    kfree(s_font.font_data);kfree(s_font.stream_cache);kfree(s_font.cells);
    kfree(s_font.entries);kfree(s_font.metrics);kfree(s_font.scratch);
    memset(&s_font,0,sizeof s_font);s_font.font_fd=-1;
}

static bool begin(const char *path,unsigned pixel_size,bool prefer_resident) {
    if(!path)path=OTFONT_DEFAULT_PATH;
    if(pixel_size<12||pixel_size>48)return false;
    if(s_font.users) {
        if(s_font.pixel_size!=pixel_size||FT_Set_Pixel_Sizes(s_font.face,0,pixel_size))return false;
        s_font.users++;return true;
    }
    uint64_t started=hal_time_now_us();
    s_font.font_fd=-1;
    vfs_info_t st;
    if(vfs_stat(path,&st)<0||st.type!=FS_TYPE_FILE||!st.size||st.size>OTFONT_MAX_FILE)goto fail;
    s_font.memory.user=0;s_font.memory.alloc=ft_alloc;s_font.memory.free=ft_free;s_font.memory.realloc=ft_realloc;
    if(FT_New_Library(&s_font.memory,&s_font.library))goto fail;
    FT_Add_Default_Modules(s_font.library);
    if(prefer_resident&&open_resident(path,st.size)&&
       !FT_New_Memory_Face(s_font.library,s_font.font_data,st.size,0,&s_font.face)) {
        s_font.font_bytes=st.size;
    } else {
        kfree(s_font.font_data);s_font.font_data=0;s_font.io_bytes=0;s_font.io_reads=0;
        if(!open_stream(path))goto fail;
        FT_Open_Args args;memset(&args,0,sizeof args);args.flags=FT_OPEN_STREAM;args.stream=&s_font.stream;
        if(FT_Open_Face(s_font.library,&args,0,&s_font.face))goto fail;
    }
    if(FT_Select_Charmap(s_font.face,FT_ENCODING_UNICODE)||FT_Set_Pixel_Sizes(s_font.face,0,pixel_size))goto fail;
    s_font.entries=font_alloc(sizeof(cache_entry)*OTFONT_CACHE_SLOTS);
    s_font.metrics=font_alloc(sizeof(metric_entry)*OTFONT_METRIC_SLOTS);
    s_font.cells=font_alloc(OTFONT_CELL_BYTES*OTFONT_CACHE_SLOTS);
    s_font.scratch=font_alloc(OTFONT_MAX_GLYPH*OTFONT_MAX_GLYPH*sizeof(uint16_t));
    if(!s_font.entries||!s_font.metrics||!s_font.cells||!s_font.scratch)goto fail;
    memset(s_font.entries,0,sizeof(cache_entry)*OTFONT_CACHE_SLOTS);
    memset(s_font.metrics,0,sizeof(metric_entry)*OTFONT_METRIC_SLOTS);
    s_font.pixel_size=pixel_size;s_font.users=1;
    s_font.load_ms=(uint32_t)((hal_time_now_us()-started)/1000u);
    unsigned source_cache=s_font.font_data?s_font.font_bytes:OTFONT_STREAM_CACHE;
    kprintf("otfont: opened %u-byte OpenType face at %u px in %u ms; %s %u KiB/%u ops; cache %u KiB\n",
            s_font.font_bytes,pixel_size,s_font.load_ms,s_font.font_data?"resident":"stream",
            s_font.io_bytes/1024u,s_font.io_reads,
            (unsigned)((source_cache+OTFONT_CELL_BYTES*OTFONT_CACHE_SLOTS)/1024u));
    return true;
fail:
    release_all();return false;
}

bool otfont_begin(const char *path,unsigned pixel_size) {return begin(path,pixel_size,false);}
bool otfont_begin_resident(const char *path,unsigned pixel_size) {return begin(path,pixel_size,true);}

void otfont_end(void) {
    if(s_font.users&&!--s_font.users)release_all();
}

bool otfont_set_pixel_size(unsigned pixel_size) {
    if(!s_font.users||pixel_size<12||pixel_size>48)return false;
    if(pixel_size==s_font.pixel_size)return true;
    if(FT_Set_Pixel_Sizes(s_font.face,0,pixel_size))return false;
    memset(s_font.entries,0,sizeof(cache_entry)*OTFONT_CACHE_SLOTS);
    memset(s_font.metrics,0,sizeof(metric_entry)*OTFONT_METRIC_SLOTS);
    s_font.pixel_size=pixel_size;return true;
}

static uint8_t *cell(unsigned slot) {return s_font.cells+slot*OTFONT_CELL_BYTES;}

static metric_entry *metric_slot(FT_UInt index,bool count_hit)
{
    unsigned set=(index*2654435761u)&(OTFONT_METRIC_SETS-1u);
    metric_entry *entries=s_font.metrics+set*OTFONT_METRIC_WAYS;
    metric_entry *victim=&entries[0];
    for(unsigned i=0;i<OTFONT_METRIC_WAYS;i++) {
        metric_entry *e=&entries[i];
        if(e->valid&&e->glyph==index) {
            e->stamp=++s_font.metric_stamp;
            if(count_hit)s_font.metric_hits++;
            return e;
        }
        if(!e->valid)return e;
        if(e->stamp<victim->stamp)victim=e;
    }
    return victim;
}

static metric_entry *metric_store(FT_UInt index,uint16_t advance,uint16_t vadvance)
{
    metric_entry *e=metric_slot(index,false);
    e->glyph=index;e->advance=advance;e->vadvance=vadvance;
    e->stamp=++s_font.metric_stamp;e->valid=1;
    return e;
}

static metric_entry *glyph_metrics(FT_UInt index)
{
    if(!index)return 0;
    metric_entry *e=metric_slot(index,true);
    if(e->valid&&e->glyph==index)return e;
    FT_Fixed horizontal=0,vertical=0;
    if(FT_Get_Advance(s_font.face,index,FT_LOAD_NO_HINTING,&horizontal))return 0;
    if(FT_Get_Advance(s_font.face,index,FT_LOAD_NO_HINTING|FT_LOAD_VERTICAL_LAYOUT,&vertical))return 0;
    uint16_t advance=(uint16_t)((horizontal+0x8000L)>>16);
    uint16_t vadvance=(uint16_t)((vertical+0x8000L)>>16);
    if(!vadvance)vadvance=(uint16_t)s_font.pixel_size;
    s_font.metric_misses++;
    return metric_store(index,advance,vadvance);
}

static cache_entry *glyph_index(FT_UInt index,unsigned *slot_out) {
    if(!index)return 0;
    unsigned victim=0;uint32_t oldest=UINT32_MAX;
    for(unsigned i=0;i<OTFONT_CACHE_SLOTS;i++) {
        cache_entry *e=&s_font.entries[i];
        if(e->valid&&e->glyph==index&&e->pixel_size==s_font.pixel_size) {
            e->stamp=++s_font.stamp;s_font.hits++;*slot_out=i;return e;
        }
        if(!e->valid){victim=i;oldest=0;break;}
        if(e->stamp<oldest){oldest=e->stamp;victim=i;}
    }
    uint64_t started=hal_time_now_us();
    if(FT_Load_Glyph(s_font.face,index,FT_LOAD_DEFAULT|FT_LOAD_NO_BITMAP) ||
       FT_Render_Glyph(s_font.face->glyph,FT_RENDER_MODE_NORMAL))return 0;
    FT_GlyphSlot g=s_font.face->glyph;FT_Bitmap *b=&g->bitmap;
    if(b->pixel_mode!=FT_PIXEL_MODE_GRAY||b->width>OTFONT_MAX_GLYPH||b->rows>OTFONT_MAX_GLYPH)return 0;
    cache_entry *e=&s_font.entries[victim];uint8_t *packed=cell(victim);
    unsigned pixels=(unsigned)b->width*b->rows;memset(packed,0,(pixels+1)/2);
    for(unsigned y=0;y<b->rows;y++) {
        const uint8_t *row=b->buffer+(b->pitch>=0?y:(b->rows-1-y))*(unsigned)(b->pitch>=0?b->pitch:-b->pitch);
        for(unsigned x=0;x<b->width;x++) {
            unsigned p=y*b->width+x,a=(row[x]+8)/17;
            if(p&1)packed[p/2]|=(uint8_t)a;else packed[p/2]=(uint8_t)(a<<4);
        }
    }
    e->glyph=index;e->pixel_size=(uint16_t)s_font.pixel_size;
    e->advance=(uint16_t)((g->advance.x+32)>>6);e->left=(int16_t)g->bitmap_left;e->top=(int16_t)g->bitmap_top;
    e->vadvance=(uint16_t)((g->metrics.vertAdvance+32)>>6);
    if(!e->vadvance)e->vadvance=(uint16_t)s_font.pixel_size;
    e->width=(uint8_t)b->width;e->height=(uint8_t)b->rows;e->stamp=++s_font.stamp;e->valid=1;
    metric_store(index,e->advance,e->vadvance);
    s_font.misses++;s_font.raster_us+=hal_time_now_us()-started;*slot_out=victim;return e;
}

static cache_entry *glyph(uint32_t cp,unsigned *slot_out) {
    return glyph_index(FT_Get_Char_Index(s_font.face,cp),slot_out);
}

static FT_UInt vertical_glyph_index(uint32_t cp) {
    unsigned lo=0,hi=(unsigned)(sizeof bizud_vertical_map/sizeof bizud_vertical_map[0]);
    while(lo<hi){unsigned mid=(lo+hi)/2;if(bizud_vertical_map[mid].cp<cp)lo=mid+1;else hi=mid;}
    if(lo<(unsigned)(sizeof bizud_vertical_map/sizeof bizud_vertical_map[0])&&
       bizud_vertical_map[lo].cp==cp)return bizud_vertical_map[lo].glyph;
    return FT_Get_Char_Index(s_font.face,cp);
}

int otfont_width(uint32_t cp) {
    if(!s_font.users)return 0;
    metric_entry *e=glyph_metrics(FT_Get_Char_Index(s_font.face,cp));
    return e?e->advance:(int)s_font.pixel_size;
}

static uint16_t blend(uint16_t f,uint16_t b,unsigned a) {
    unsigned r=(((f>>11)*a+(b>>11)*(15-a)+7)/15);
    unsigned g=((((f>>5)&63)*a+((b>>5)&63)*(15-a)+7)/15);
    unsigned bl=(((f&31)*a+(b&31)*(15-a)+7)/15);
    return (uint16_t)((r<<11)|(g<<5)|bl);
}

int otfont_draw(int x,int baseline,uint32_t cp,uint16_t fg,uint16_t bg) {
    if(!s_font.users)return 0;
    unsigned slot=0;cache_entry *e=glyph(cp,&slot);
    if(!e){display_draw_rect(x+3,baseline-(int)s_font.pixel_size+3,(int)s_font.pixel_size-5,(int)s_font.pixel_size-3,fg);return (int)s_font.pixel_size;}
    unsigned pixels=(unsigned)e->width*e->height;uint8_t *packed=cell(slot);
    for(unsigned p=0;p<pixels;p++){unsigned byte=packed[p/2],a=(p&1)?byte&15:byte>>4;s_font.scratch[p]=blend(fg,bg,a);}
    if(pixels)display_draw_bitmap(x+e->left,baseline-e->top,e->width,e->height,s_font.scratch);
    return e->advance;
}

int otfont_vertical_advance(uint32_t cp) {
    if(!s_font.users)return 0;
    metric_entry *e=glyph_metrics(vertical_glyph_index(cp));
    return e?e->vadvance:(int)s_font.pixel_size;
}

int otfont_draw_vertical(int x,int top,uint32_t cp,uint16_t fg,uint16_t bg) {
    if(!s_font.users)return 0;
    unsigned slot=0;cache_entry *e=glyph_index(vertical_glyph_index(cp),&slot);
    if(!e){display_draw_rect(x+3,top+3,(int)s_font.pixel_size-5,(int)s_font.pixel_size-3,fg);return (int)s_font.pixel_size;}
    unsigned pixels=(unsigned)e->width*e->height;uint8_t *packed=cell(slot);
    for(unsigned p=0;p<pixels;p++){unsigned byte=packed[p/2],a=(p&1)?byte&15:byte>>4;s_font.scratch[p]=blend(fg,bg,a);}
    int baseline=top+(int)(s_font.pixel_size*21u+12u)/24;
    if(pixels)display_draw_bitmap(x+e->left,baseline-e->top,e->width,e->height,s_font.scratch);
    return e->vadvance;
}

int otfont_text(int x,int baseline,const char *text,int right,uint16_t fg,uint16_t bg) {
    while(*text){uint32_t cp=bizud_decode(&text);int advance=otfont_width(cp);if(x+advance>right)break;x+=otfont_draw(x,baseline,cp,fg,bg);}
    return x;
}

void otfont_get_stats(otfont_stats *out) {
    if(!out)return;
    out->font_bytes=s_font.font_bytes;out->resident_bytes=s_font.font_data?s_font.font_bytes:0;
    out->load_ms=s_font.load_ms;
    out->glyph_hits=s_font.hits;out->glyph_misses=s_font.misses;out->raster_us=s_font.raster_us;
    out->metric_hits=s_font.metric_hits;out->metric_misses=s_font.metric_misses;
    out->io_bytes=s_font.io_bytes;out->io_reads=s_font.io_reads;
    size_t source_cache=s_font.font_data?s_font.font_bytes:OTFONT_STREAM_CACHE;
    out->cache_bytes=s_font.entries?source_cache+sizeof(cache_entry)*OTFONT_CACHE_SLOTS+
        sizeof(metric_entry)*OTFONT_METRIC_SLOTS+
        OTFONT_CELL_BYTES*OTFONT_CACHE_SLOTS+OTFONT_MAX_GLYPH*OTFONT_MAX_GLYPH*sizeof(uint16_t):0;
}
#endif

