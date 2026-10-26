#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include "fontbench.h"
#include "../../drivers/lcd/bizud.h"
#include "../../drivers/lcd/fbcon.h"
#include "../../drivers/lcd/otfont.h"
#include "../tui/dqwin.h"
#include <m5os/display.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>

#define BG RGB565(248,247,242)
#define FG RGB565(36,39,42)
#define ACCENT RGB565(25,85,115)
#define GUIDE RGB565(205,215,218)
#define LEFT 48
#define RIGHT (DISPLAY_WIDTH-48)
#define TEXT_RIGHT 850

static const char *sample1="日本語と English を、無理なく読める大きさで。";
static const char *sample2="文章を書く道具として、文字の形と間隔を比較します。0123456789";

static void atlas_line(int y,const char *text) {bizud_text(LEFT,y,text,TEXT_RIGHT,FG,BG);}
static void vertical_column(int x,int y,const char *text,bool outline) {
    while(*text) {
        uint32_t cp=bizud_decode(&text);
        int advance=outline?otfont_draw_vertical(x,y,cp,FG,BG):bizud_draw_vertical(x,y,cp,FG,BG);
        y+=advance+5;
    }
}

bool fontbench_run(unsigned pixel_size) {
    static uint8_t owner;if(!display_try_claim(&owner))return false;
    uint16_t *frame=kmalloc_psram(DISPLAY_BUFFER_BYTES);bool atlas=false,outline=false;
    if(!frame)goto done;
    if(!bizud_begin())goto done;
    atlas=true;
    if(!otfont_begin(OTFONT_DEFAULT_PATH,pixel_size))goto done;
    outline=true;
    fbcon_pause(true);display_set_target(frame);display_fill_rect(0,0,DISPLAY_WIDTH,DISPLAY_HEIGHT,BG);
    bizud_text(LEFT,30,"BIZ UD: 固定24pxアトラス / FreeType実行時ラスタライズ",RIGHT,ACCENT,BG);
    display_fill_rect(LEFT,40,RIGHT-LEFT,1,ACCENT);
    bizud_text(LEFT,82,"固定アトラス 24px",RIGHT,ACCENT,BG);
    display_fill_rect(LEFT,116,RIGHT-LEFT,1,GUIDE);atlas_line(116,sample1);
    display_fill_rect(LEFT,162,RIGHT-LEFT,1,GUIDE);atlas_line(162,sample2);
    bizud_text(LEFT,232,"FreeType（ヒンティングあり）",RIGHT,ACCENT,BG);
    uint64_t metric=hal_time_now_us();const char *scan=sample1;
    while(*scan)otfont_width(bizud_decode(&scan));
    scan=sample2;while(*scan)otfont_width(bizud_decode(&scan));
    scan=sample1;while(*scan)otfont_vertical_advance(bizud_decode(&scan));
    uint32_t metric_us=(uint32_t)(hal_time_now_us()-metric);
    uint64_t first=hal_time_now_us();
    display_fill_rect(LEFT,270,TEXT_RIGHT-LEFT,1,GUIDE);otfont_text(LEFT,270,sample1,TEXT_RIGHT,FG,BG);
    display_fill_rect(LEFT,320,TEXT_RIGHT-LEFT,1,GUIDE);otfont_text(LEFT,320,sample2,TEXT_RIGHT,FG,BG);
    uint32_t first_us=(uint32_t)(hal_time_now_us()-first);
    uint64_t warm=hal_time_now_us();
    display_fill_rect(LEFT,400,TEXT_RIGHT-LEFT,1,GUIDE);otfont_text(LEFT,400,sample1,TEXT_RIGHT,FG,BG);
    display_fill_rect(LEFT,450,TEXT_RIGHT-LEFT,1,GUIDE);otfont_text(LEFT,450,sample2,TEXT_RIGHT,FG,BG);
    uint32_t warm_us=(uint32_t)(hal_time_now_us()-warm);
    otfont_stats st;otfont_get_stats(&st);char line[192];
    ksnprintf(line,sizeof line,"%upx  初回描画 %u ms / 温間描画 %u ms / miss %u hit %u",
              pixel_size,first_us/1000u,warm_us/1000u,st.glyph_misses,st.glyph_hits);
    bizud_text(LEFT,536,line,RIGHT,ACCENT,BG);
    ksnprintf(line,sizeof line,"TTF %u KiB / read %u KiB (%u ms) / cache %u KiB / Qで戻る",
              st.font_bytes/1024u,st.io_bytes/1024u,st.load_ms,(unsigned)(st.cache_bytes/1024u));
    bizud_text(LEFT,578,line,TEXT_RIGHT,FG,BG);
    bizud_text(900,82,"固定",1000,ACCENT,BG);bizud_text(1040,82,"FT",RIGHT,ACCENT,BG);
    uint64_t vertical=hal_time_now_us();
    vertical_column(920,110,"日本語を書く。「括弧」、長音ー。",false);
    vertical_column(1050,110,"日本語を書く。「括弧」、長音ー。",true);
    uint32_t vertical_us=(uint32_t)(hal_time_now_us()-vertical);
    display_present(0,0,DISPLAY_WIDTH,DISPLAY_HEIGHT);display_set_target(0);
    kprintf("FONTBENCH %u px: load=%u ms metric=%u us first=%u us warm=%u us misses=%u hits=%u raster=%u us font=%u resident=%u cache=%u io=%u reads=%u metrics=%u/%u\n",
            pixel_size,st.load_ms,metric_us,first_us,warm_us,st.glyph_misses,st.glyph_hits,
            (uint32_t)st.raster_us,st.font_bytes,st.resident_bytes,(uint32_t)st.cache_bytes,st.io_bytes,st.io_reads,
            st.metric_misses,st.metric_hits);
    kprintf("VERTBENCH %u px: draw=%u us\n",pixel_size,vertical_us);
    for(;;){int key=dq_pollkey();if(key=='q'||key=='Q'||key==DQK_ESC||key==3)break;task_sleep(10);}
done:
    display_set_target(0);if(outline)otfont_end();if(atlas)bizud_end();
    if(atlas)bizud_trim();
    if(frame)kfree(frame);
    fbcon_pause(false);display_release_claim(&owner);return outline;
}
#endif

