#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include "write.h"
#include "../../drivers/lcd/bizud.h"
#include "../../drivers/lcd/fbcon.h"
#include "../scheme/buffer.h"
#include "../tui/dqwin.h"
#include <m5os/display.h>
#include <m5os/errno.h>
#include <m5os/ime.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/vfs.h>
#include <string.h>
#define BG RGB565(248,247,242)
#define FG RGB565(36,39,42)
#define ACCENT RGB565(25,85,115)
#define LEFT 48
#define RIGHT (DISPLAY_WIDTH-48)
#define PITCH 36
#define ROWS ((DISPLAY_HEIGHT-250)/PITCH)
#define CTRL(c) ((c)-'a'+1)
typedef struct { scheme_buffer b; size_t starts[SCHEME_SOURCE_CAP+2]; int rows,top; bool confirm; uint16_t *frame; char status[120]; } writer;
static uint32_t at(writer *w,size_t p) { const char *s=w->b.text+p; return bizud_decode(&s); }
static int width(writer *w,size_t p) { uint32_t cp=at(w,p); return cp=='\t'?56:cp=='\r'?0:bizud_width(cp); }
static void layout(writer *w) {
    int x=LEFT; w->rows=1; w->starts[0]=0;
    for(size_t p=0;p<w->b.length;) {
        uint32_t cp=at(w,p);int adv=width(w,p);
        if(cp!='\n' && x+adv>RIGHT) { w->starts[w->rows++]=p;x=LEFT; }
        p=scheme_buffer_next(&w->b,p);
        if(cp=='\n') { w->starts[w->rows++]=p;x=LEFT; } else x+=adv;
    }
    w->starts[w->rows]=w->b.length;
}
static int cursor_row(writer *w) { int r=0;while(r+1<w->rows && w->starts[r+1]<=w->b.cursor)r++;return r; }
static int cursor_x(writer *w,int row) { int x=LEFT;for(size_t p=w->starts[row];p<w->b.cursor;p=scheme_buffer_next(&w->b,p))x+=width(w,p);return x; }
static void move_vertical(writer *w,int delta) {
    int row=cursor_row(w),x=cursor_x(w,row),target=row+delta;
    if(target<0)target=0;
    if(target>=w->rows)target=w->rows-1;
    size_t p=w->starts[target];int dx=LEFT;
    while(p<w->starts[target+1] && at(w,p)!='\n') {
        int a=width(w,p);if(dx+a/2>=x)break;dx+=a;p=scheme_buffer_next(&w->b,p);
    }
    if(target+1<w->rows && p==w->starts[target+1] && p>w->starts[target])p=scheme_buffer_prev(&w->b,p);
    w->b.cursor=p;
}
static void paint(writer *w,const char *path,const ime_view *view) {
    int cr=cursor_row(w);
    if(cr<w->top)w->top=cr;
    if(cr>=w->top+ROWS)w->top=cr-ROWS+1;
    display_set_target(w->frame);
    display_fill_rect(0,0,DISPLAY_WIDTH,DISPLAY_HEIGHT,BG);
    char header[160];ksnprintf(header,sizeof header,"WRITE  %s%s",path,w->b.modified?" *":"");
    bizud_text(LEFT,38,header,RIGHT,ACCENT,BG);
    display_fill_rect(LEFT,54,RIGHT-LEFT,1,ACCENT);
    for(int r=w->top;r<w->rows && r<w->top+ROWS;r++) {
        int x=LEFT,y=92+(r-w->top)*PITCH;
        for(size_t p=w->starts[r];p<w->starts[r+1];p=scheme_buffer_next(&w->b,p)) {
            uint32_t cp=at(w,p);if(cp=='\n')break;
            if(cp!='\t' && cp!='\r')bizud_draw(x,y,cp,FG,BG);
            x+=width(w,p);
        }
    }
    display_fill_rect(cursor_x(w,cr),92+(cr-w->top)*PITCH-26,2,31,ACCENT);
    int y=DISPLAY_HEIGHT-155;
    display_fill_rect(LEFT,y-12,RIGHT-LEFT,1,ACCENT);
    int x=LEFT;
    if(view->active) {
        for(int n=0;n<view->count;n++) {
            int a=bizud_width(view->text[n]);if(x+a>RIGHT)break;
            bizud_draw(x,y+25,view->text[n],FG,BG);
            if(view->styles[n])display_fill_rect(x,y+29,a,2,ACCENT);
            x+=a;
        }
        x=LEFT;
        for(int n=0;n<view->candidate_count;n++) {
            char candidate[220];ksnprintf(candidate,sizeof candidate,"%d:%s  ",view->numbers[n],view->candidates[n]);
            uint16_t fg=n==view->selected?BG:FG,bg=n==view->selected?ACCENT:BG;
            int len=0;const char *p=candidate;while(*p)len+=bizud_width(bizud_decode(&p));
            if(x+len>RIGHT)break;
            display_fill_rect(x,y+40,len,36,bg);bizud_text(x,y+68,candidate,RIGHT,fg,bg);x+=len;
        }
    } else bizud_text(LEFT,y+25,"BIZ UD 24px / 日本語入力: Ctrl-Space",RIGHT,ACCENT,BG);
    bizud_text(LEFT,DISPLAY_HEIGHT-37,"^S 保存   ^Q 終了   ^Z 戻す   矢印 移動",RIGHT,FG,BG);
    if(w->status[0])bizud_text(LEFT,DISPLAY_HEIGHT-4,w->status,RIGHT,ACCENT,BG);
    display_present(0,0,DISPLAY_WIDTH,DISPLAY_HEIGHT);
    display_set_target(NULL);
}
static bool save(writer *w,const char *path) {
    int rc=vfs_replace_file(path,w->b.text,w->b.length,NULL);
    if(rc<0)ksnprintf(w->status,sizeof w->status,"保存に失敗しました (%d)",rc);
    else { w->b.modified=false;ksnprintf(w->status,sizeof w->status,"保存しました"); }
    return rc>=0;
}
bool write_file(const char *path) {
    writer *w=kmalloc_psram(sizeof *w);if(!w)return false;
    memset(w,0,sizeof *w);vfs_info_t st;
    int stat=vfs_stat(path,&st);
    if(stat==ERR_OK) {
        if(st.type!=FS_TYPE_FILE || st.size>SCHEME_SOURCE_CAP){kfree(w);return false;}
        int fd=vfs_open(path,VFS_O_READ);if(fd<0){kfree(w);return false;}
        size_t n=0;int got=0;
        while(n<st.size && (got=vfs_read(fd,w->b.text+n,st.size-n))>0)n+=(size_t)got;
        vfs_close(fd);
        if(got<0 || n!=st.size || !scheme_utf8_text(w->b.text,n)){kfree(w);return false;}
        w->b.length=n;
    } else if(stat!=ERR_NOENT){kfree(w);return false;}
    if(!display_try_claim(w)){kfree(w);return false;}
    w->frame=kmalloc_psram(DISPLAY_BUFFER_BYTES);
    if(!w->frame || !bizud_begin()){kfree(w->frame);display_release_claim(w);kfree(w);return false;}
    fbcon_pause(true);ime_set_app_view(true);
    bool run=true,ok=true,redraw=true;uint32_t revision=~0u;
    ime_view *view=kmalloc_psram(sizeof *view);
    if(!view){ime_set_app_view(false);fbcon_pause(false);display_release_claim(w);bizud_end();kfree(w->frame);kfree(w);return false;}
    while(run) {
        ime_read_view(view);
        if(redraw || revision!=view->revision) {layout(w);paint(w,path,view);revision=view->revision;redraw=false;}
        int k=dq_pollkey();if(k==DQK_NONE){task_sleep(10);continue;}
        redraw=true;
        if(k==CTRL('q')) { if(!w->b.modified || w->confirm)run=false;else { w->confirm=true;ksnprintf(w->status,sizeof w->status,"未保存です。もう一度 Ctrl-Q で終了 / Ctrl-S で保存"); } continue; }
        w->confirm=false;w->status[0]=0;
        switch(k) {
        case CTRL('s'): save(w,path);break;
        case CTRL('z'): scheme_buffer_undo(&w->b);break;
        case DQK_LEFT:w->b.cursor=scheme_buffer_prev(&w->b,w->b.cursor);break;
        case DQK_RIGHT:w->b.cursor=scheme_buffer_next(&w->b,w->b.cursor);break;
        case DQK_UP:move_vertical(w,-1);break;
        case DQK_DOWN:move_vertical(w,1);break;
        case DQK_PGUP:move_vertical(w,-ROWS);break;
        case DQK_PGDN:move_vertical(w,ROWS);break;
        case DQK_HOME:while(w->b.cursor && w->b.text[w->b.cursor-1]!='\n')w->b.cursor=scheme_buffer_prev(&w->b,w->b.cursor);break;
        case DQK_END:while(w->b.cursor<w->b.length && w->b.text[w->b.cursor]!='\n')w->b.cursor=scheme_buffer_next(&w->b,w->b.cursor);break;
        case DQK_BACKSPACE:scheme_buffer_delete(&w->b,true);break;
        case DQK_DEL:scheme_buffer_delete(&w->b,false);break;
        default: {
            char bytes[4];size_t n=1;bytes[0]=(char)k;
            if(k==DQK_ENTER)bytes[0]='\n';
            else if(k>=0xc2 && k<=0xf4) {n=k>=0xf0?4:k>=0xe0?3:2;for(size_t i=1;i<n;i++)bytes[i]=(char)dq_getkey();}
            else if(k!='\t' && (k<32 || k>=127))break;
            if(!scheme_buffer_insert(&w->b,bytes,n))ksnprintf(w->status,sizeof w->status,"入力できません（最大16 KB）");
            break;
        }}
    }
    ime_set_app_view(false);ime_clear_anchor();fbcon_pause(false);display_release_claim(w);kfree(view);bizud_end();kfree(w->frame);kfree(w);return ok;
}
#endif
