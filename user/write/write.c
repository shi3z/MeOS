#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include "write.h"
#include "../../drivers/lcd/bizud.h"
#include "../../drivers/lcd/fbcon.h"
#include "../../drivers/lcd/otfont.h"
#include "../editor/bizime.h"
#include "../scheme/buffer.h"
#include "../tui/dqwin.h"
#include <m5os/display.h>
#include <m5os/errno.h>
#include <m5os/ime.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/sdroot.h>
#include <m5os/task.h>
#include <m5os/vfs.h>
#include <string.h>
#define BG RGB565(248,247,242)
#define FG RGB565(36,39,42)
#define ACCENT RGB565(25,85,115)
#define LEFT 48
#define RIGHT (DISPLAY_WIDTH-48)
#define H_BOTTOM (DISPLAY_HEIGHT-12)
#define V_BOTTOM (DISPLAY_HEIGHT-36)
#define CTRL(c) ((c)-'a'+1)
typedef struct {
    scheme_buffer b;
    size_t starts[SCHEME_SOURCE_CAP+2];
    int lines,top;
    bool confirm,vertical,outline;
    unsigned font_px;
    uint16_t *frame;
    char status[120];
    char commit[IME_COMMIT_BYTES];
    uint32_t diversity[512];
} writer;

static void *editor_alloc(size_t bytes) {
    void *p=kmalloc_psram(bytes);
    if(!p&&bizud_trim())p=kmalloc_psram(bytes);
    if(!p&&sdroot_asset_cache_trim())p=kmalloc_psram(bytes);
    return p;
}
static int header_baseline(writer *w) {return (int)w->font_px+6;}
static int header_rule(writer *w) {return header_baseline(w)+10;}
static int h_baseline(writer *w) {return header_rule(w)+30;}
static int h_pitch(writer *w) {return (int)w->font_px+12;}
static int h_rows(writer *w) {return (H_BOTTOM-h_baseline(w))/h_pitch(w)+1;}
static int v_top(writer *w) {return header_rule(w)+8;}
static int v_column_pitch(writer *w) {return (int)w->font_px+16;}
static int v_tracking(writer *w) {return ((int)w->font_px+3)/4;}
static int v_columns(writer *w) {return (RIGHT-LEFT-(int)w->font_px)/v_column_pitch(w)+1;}
static uint32_t at(writer *w,size_t p) {const char *s=w->b.text+p;return bizud_decode(&s);}
static int font_width(writer *w,uint32_t cp) {return w->outline?otfont_width(cp):bizud_width(cp);}
static int font_draw(writer *w,int x,int baseline,uint32_t cp,uint16_t fg,uint16_t bg) {
    return w->outline?otfont_draw(x,baseline,cp,fg,bg):bizud_draw(x,baseline,cp,fg,bg);
}
static int font_vadvance(writer *w,uint32_t cp) {
    return w->outline?otfont_vertical_advance(cp):bizud_vertical_advance(cp);
}
static int font_draw_vertical(writer *w,int x,int top,uint32_t cp,uint16_t fg,uint16_t bg) {
    return w->outline?otfont_draw_vertical(x,top,cp,fg,bg):bizud_draw_vertical(x,top,cp,fg,bg);
}
static int font_text(writer *w,int x,int baseline,const char *text,int right,uint16_t fg,uint16_t bg) {
    return w->outline?otfont_text(x,baseline,text,right,fg,bg):bizud_text(x,baseline,text,right,fg,bg);
}
static int hwidth(writer *w,size_t p) {uint32_t cp=at(w,p);return cp=='\t'?56:cp=='\r'?0:font_width(w,cp);}
static int vstep_cp(writer *w,uint32_t cp) {return font_vadvance(w,cp)+v_tracking(w);}
static int vstep(writer *w,size_t p) {return vstep_cp(w,at(w,p));}

static bool prohibited_at_column_start(uint32_t cp) {
    switch(cp) {
    case 0x2019:case 0x201d:case 0x3001:case 0x3002:case 0x3005:
    case 0x3009:case 0x300b:case 0x300d:case 0x300f:case 0x3011:
    case 0x3015:case 0x3017:case 0x3019:case 0x301b:case 0x309d:case 0x309e:
    case 0x30fb:case 0x30fc:case 0x30fd:case 0x30fe:
    case 0xff01:case 0xff09:case 0xff0c:case 0xff0e:case 0xff1a:case 0xff1b:
    case 0xff1f:case 0xff3d:case 0xff5d:
    case 0x3041:case 0x3043:case 0x3045:case 0x3047:case 0x3049:
    case 0x3063:case 0x3083:case 0x3085:case 0x3087:case 0x308e:
    case 0x30a1:case 0x30a3:case 0x30a5:case 0x30a7:case 0x30a9:
    case 0x30c3:case 0x30e3:case 0x30e5:case 0x30e7:case 0x30ee:
    case 0x30f5:case 0x30f6:return true;
    default:return false;
    }
}
static bool prohibited_at_column_end(uint32_t cp) {
    switch(cp) {
    case 0x2018:case 0x201c:case 0x3008:case 0x300a:case 0x300c:
    case 0x300e:case 0x3010:case 0x3014:case 0x3016:case 0x3018:
    case 0x301a:case 0xff08:case 0xff3b:case 0xff5b:return true;
    default:return false;
    }
}

static void layout_horizontal(writer *w) {
    int x=LEFT;w->lines=1;w->starts[0]=0;
    for(size_t p=0;p<w->b.length;) {
        uint32_t cp=at(w,p);int advance=hwidth(w,p);
        if(cp!='\n'&&x+advance>RIGHT){w->starts[w->lines++]=p;x=LEFT;}
        p=scheme_buffer_next(&w->b,p);
        if(cp=='\n'){w->starts[w->lines++]=p;x=LEFT;}else x+=advance;
    }
    w->starts[w->lines]=w->b.length;
}

static void layout_vertical(writer *w) {
    int y=v_top(w);w->lines=1;w->starts[0]=0;
    for(size_t p=0;p<w->b.length;) {
        uint32_t cp=at(w,p);int step=vstep_cp(w,cp);size_t next=scheme_buffer_next(&w->b,p);
        bool wrap=cp!='\n'&&y+step>V_BOTTOM;
        if(!wrap&&cp!='\n'&&prohibited_at_column_end(cp)&&next<w->b.length) {
            uint32_t following=at(w,next);
            if(following!='\n'&&y+step+vstep_cp(w,following)>V_BOTTOM)wrap=true;
        }
        /* One closing mark may hang into the reserved lower margin instead of
         * becoming the first character of the next column. */
        if(wrap&&prohibited_at_column_start(cp)&&y<=V_BOTTOM)wrap=false;
        if(wrap){w->starts[w->lines++]=p;y=v_top(w);}
        p=next;
        if(cp=='\n'){w->starts[w->lines++]=p;y=v_top(w);}else y+=step;
    }
    w->starts[w->lines]=w->b.length;
}
static void layout(writer *w) {if(w->vertical)layout_vertical(w);else layout_horizontal(w);}
static int cursor_line(writer *w) {int n=0;while(n+1<w->lines&&w->starts[n+1]<=w->b.cursor)n++;return n;}
static int cursor_hx(writer *w,int line) {int x=LEFT;for(size_t p=w->starts[line];p<w->b.cursor;p=scheme_buffer_next(&w->b,p))x+=hwidth(w,p);return x;}
static int cursor_vy(writer *w,int line) {int y=v_top(w);for(size_t p=w->starts[line];p<w->b.cursor;p=scheme_buffer_next(&w->b,p))y+=vstep(w,p);return y;}

static void move_across_lines(writer *w,int delta) {
    int line=cursor_line(w),target=line+delta;
    if(target<0)target=0;
    if(target>=w->lines)target=w->lines-1;
    size_t p=w->starts[target];
    if(w->vertical) {
        int wanted=cursor_vy(w,line),y=v_top(w);
        while(p<w->starts[target+1]&&at(w,p)!='\n') {
            int step=vstep(w,p);if(y+step/2>=wanted)break;y+=step;p=scheme_buffer_next(&w->b,p);
        }
    } else {
        int wanted=cursor_hx(w,line),x=LEFT;
        while(p<w->starts[target+1]&&at(w,p)!='\n') {
            int advance=hwidth(w,p);if(x+advance/2>=wanted)break;x+=advance;p=scheme_buffer_next(&w->b,p);
        }
    }
    if(target+1<w->lines&&p==w->starts[target+1]&&p>w->starts[target])p=scheme_buffer_prev(&w->b,p);
    w->b.cursor=p;
}

static void move_inside_column(writer *w,int delta) {
    int column=cursor_line(w);size_t begin=w->starts[column],end=w->starts[column+1];
    if(delta<0) {if(w->b.cursor>begin)w->b.cursor=scheme_buffer_prev(&w->b,w->b.cursor);}
    else if(w->b.cursor<end) {
        size_t next=scheme_buffer_next(&w->b,w->b.cursor);
        if(next<end||(column==w->lines-1&&next==end))w->b.cursor=next;
    }
}

static void draw_horizontal_tail(writer *w,bizime_point point) {
    int bottom=H_BOTTOM,x=point.x,y=point.baseline;
    for(size_t p=w->b.cursor;p<w->b.length;p=scheme_buffer_next(&w->b,p)) {
        uint32_t cp=at(w,p);if(cp=='\n'){x=LEFT;y+=h_pitch(w);continue;}
        int advance=hwidth(w,p);if(x+advance>RIGHT){x=LEFT;y+=h_pitch(w);}
        if(y>bottom)break;
        if(cp!='\t'&&cp!='\r')font_draw(w,x,y,cp,FG,BG);
        x+=advance;
    }
}

static void draw_vertical_tail(writer *w,bizime_vpoint point) {
    int x=point.x,y=point.y;
    for(size_t p=w->b.cursor;p<w->b.length;) {
        uint32_t cp=at(w,p);size_t next=scheme_buffer_next(&w->b,p);
        if(cp=='\n'){x-=v_column_pitch(w);y=v_top(w);p=next;continue;}
        int step=vstep_cp(w,cp);
        bool wrap=y+step>V_BOTTOM;
        if(!wrap&&prohibited_at_column_end(cp)&&next<w->b.length) {
            uint32_t following=at(w,next);
            if(following!='\n'&&y+step+vstep_cp(w,following)>V_BOTTOM)wrap=true;
        }
        if(wrap&&prohibited_at_column_start(cp)&&y<=V_BOTTOM)wrap=false;
        if(wrap){x-=v_column_pitch(w);y=v_top(w);}
        if(x<LEFT)break;
        if(cp!='\t'&&cp!='\r')font_draw_vertical(w,x,y,cp,FG,BG);
        y+=step;p=next;
    }
}

static void paint_horizontal(writer *w,const ime_view *view) {
    int line=cursor_line(w);
    if(line<w->top)w->top=line;
    int rows=h_rows(w);
    if(line>=w->top+rows)w->top=line-rows+1;
    for(int r=w->top;r<w->lines&&r<w->top+rows;r++) {
        int x=LEFT,y=h_baseline(w)+(r-w->top)*h_pitch(w);
        for(size_t p=w->starts[r];p<w->starts[r+1];p=scheme_buffer_next(&w->b,p)) {
            uint32_t cp=at(w,p);if(cp=='\n')break;if(cp!='\t'&&cp!='\r')font_draw(w,x,y,cp,FG,BG);x+=hwidth(w,p);
        }
    }
    int cx=cursor_hx(w,line),cy=h_baseline(w)+(line-w->top)*h_pitch(w);
    if(view->count) {
        int top=(int)w->font_px+3,box=(int)w->font_px+8;
        int bottom=H_BOTTOM,next=cy+h_pitch(w)-top;
        display_fill_rect(cx,cy-top,RIGHT-cx,box,BG);
        if(next<bottom)display_fill_rect(LEFT,next,RIGHT-LEFT,bottom-next,BG);
        bizime_point after=bizime_measure(view,cx,cy,LEFT,RIGHT,h_pitch(w));
        draw_horizontal_tail(w,after);
        bizime_draw(view,cx,cy,LEFT,RIGHT,header_rule(w)+2,H_BOTTOM,h_pitch(w),FG,BG,ACCENT);
    } else display_fill_rect(cx,cy-(int)w->font_px-2,2,(int)w->font_px+7,ACCENT);
}

static void paint_vertical(writer *w,const ime_view *view) {
    int column=cursor_line(w);
    if(column<w->top)w->top=column;
    int columns=v_columns(w),pitch=v_column_pitch(w),top=v_top(w);
    if(column>=w->top+columns)w->top=column-columns+1;
    for(int c=w->top;c<w->lines&&c<w->top+columns;c++) {
        int x=RIGHT-(int)w->font_px-(c-w->top)*pitch,y=top;
        for(size_t p=w->starts[c];p<w->starts[c+1];p=scheme_buffer_next(&w->b,p)) {
            uint32_t cp=at(w,p);if(cp=='\n')break;
            if(cp!='\t'&&cp!='\r')font_draw_vertical(w,x,y,cp,FG,BG);
            y+=vstep_cp(w,cp);
        }
    }
    int cx=RIGHT-(int)w->font_px-(column-w->top)*pitch,cy=cursor_vy(w,column);
    if(view->count) {
        if(cx>LEFT)display_fill_rect(LEFT,top,cx-LEFT,V_BOTTOM-top+(int)w->font_px+4,BG);
        display_fill_rect(cx,cy,(int)w->font_px,V_BOTTOM-cy+(int)w->font_px+4,BG);
        bizime_vpoint after=bizime_measure_vertical(view,cx,cy,top,V_BOTTOM,pitch,v_tracking(w));
        draw_vertical_tail(w,after);
        bizime_draw_vertical(view,cx,cy,LEFT,RIGHT,top,V_BOTTOM,
                             pitch,v_tracking(w),FG,BG,ACCENT);
    } else display_fill_rect(cx,cy,(int)w->font_px,2,ACCENT);
}

static void paint(writer *w,const char *path,const ime_view *view) {
    display_set_target(w->frame);display_fill_rect(0,0,DISPLAY_WIDTH,DISPLAY_HEIGHT,BG);
    char header[256];
    ksnprintf(header,sizeof header,"WRITE %s%s %upx  ^S保存 ^Q終了 ^Z戻す ^T%s F3/F4字  IME:%s%s%s  %s",
              w->vertical?"縦":"横",w->b.modified?"*":"",w->font_px,w->vertical?"横":"縦",
              ime_status(),w->status[0]?"  ":"",w->status,path);
    font_text(w,LEFT,header_baseline(w),header,RIGHT,view->active?ACCENT:FG,BG);
    display_fill_rect(LEFT,header_rule(w),RIGHT-LEFT,1,ACCENT);
    if(w->vertical)paint_vertical(w,view);else paint_horizontal(w,view);
    display_present(0,0,DISPLAY_WIDTH,DISPLAY_HEIGHT);display_set_target(NULL);
}

static void resize_font(writer *w,int direction) {
    static const unsigned sizes[]={20,24,28,32};unsigned at=0;
    while(at+1<sizeof sizes/sizeof sizes[0]&&sizes[at]<w->font_px)at++;
    if(direction<0&&at)at--;else if(direction>0&&at+1<sizeof sizes/sizeof sizes[0])at++;
    unsigned next=sizes[at];
    if(!w->outline){ksnprintf(w->status,sizeof w->status,"サイズ変更にはアウトラインフォントが必要です");return;}
    if(next==w->font_px)return;
    if(!otfont_set_pixel_size(next)){ksnprintf(w->status,sizeof w->status,"%upxへ変更できません",next);return;}
    w->font_px=next;w->top=0;bizime_set_font(true,next);
    ksnprintf(w->status,sizeof w->status,"%upxに変更しました",next);
    kprintf("write: outline %upx\n",next);
}

static bool save(writer *w,const char *path) {
    int rc=vfs_replace_file(path,w->b.text,w->b.length,NULL);
    if(rc<0)ksnprintf(w->status,sizeof w->status,"保存に失敗しました (%d)",rc);
    else {w->b.modified=false;ksnprintf(w->status,sizeof w->status,"保存しました");}
    return rc>=0;
}

static bool benefits_from_resident_font(writer *w) {
    if(w->b.length<8192)return false;
    memset(w->diversity,0,sizeof w->diversity);unsigned distinct=0;
    for(size_t p=0;p<w->b.length;) {
        uint32_t cp=at(w,p);p=scheme_buffer_next(&w->b,p);
        if(cp<0x80)continue;
        unsigned slot=(cp*2654435761u)&511u;
        while(w->diversity[slot]&&w->diversity[slot]!=cp)slot=(slot+1u)&511u;
        if(w->diversity[slot]==cp)continue;
        w->diversity[slot]=cp;
        if(++distinct>=256)return true;
    }
    return false;
}

bool write_file(const char *path) {
    writer *w=editor_alloc(sizeof *w);if(!w)return false;
    memset(w,0,sizeof *w);w->vertical=true;w->font_px=24;vfs_info_t st;
    int stat=vfs_stat(path,&st);
    if(stat==ERR_OK) {
        if(st.type!=FS_TYPE_FILE||st.size>SCHEME_SOURCE_CAP){kfree(w);return false;}
        int fd=vfs_open(path,VFS_O_READ);if(fd<0){kfree(w);return false;}
        size_t n=0;int got=0;while(n<st.size&&(got=vfs_read(fd,w->b.text+n,st.size-n))>0)n+=(size_t)got;
        vfs_close(fd);if(got<0||n!=st.size||!scheme_utf8_text(w->b.text,n)){kfree(w);return false;}w->b.length=n;
    } else if(stat!=ERR_NOENT){kfree(w);return false;}
    if(!display_try_claim(w)){kfree(w);return false;}w->frame=editor_alloc(DISPLAY_BUFFER_BYTES);
    if(!w->frame){display_release_claim(w);kfree(w);return false;}
    bool resident=benefits_from_resident_font(w);
    w->outline=resident?otfont_begin_resident(OTFONT_DEFAULT_PATH,w->font_px):
                        otfont_begin(OTFONT_DEFAULT_PATH,w->font_px);
    if(!w->outline&&!bizud_begin()){kfree(w->frame);display_release_claim(w);kfree(w);return false;}
    bizime_set_font(w->outline,w->font_px);
    kprintf("write: BIZ UD %upx %s\n",w->font_px,w->outline?"outline":"atlas fallback");
    fbcon_pause(true);ime_set_app_view(true);bool run=true,ok=true,redraw=true;uint32_t revision=~0u;
    ime_view *view=editor_alloc(sizeof *view);
    if(!view){ime_set_app_view(false);fbcon_pause(false);display_release_claim(w);bizime_set_font(false,24);
        if(w->outline)otfont_end();else bizud_end();kfree(w->frame);kfree(w);return false;}
    while(run) {
        size_t committed = ime_take_commit(w->commit, sizeof w->commit);
        if (committed) {
            w->confirm = false; w->status[0] = 0;
            if (!scheme_buffer_insert(&w->b, w->commit, committed))
                ksnprintf(w->status, sizeof w->status, "入力できません（最大16 KB）");
            redraw = true;
        }
        ime_read_view(view);if(redraw||revision!=view->revision){layout(w);paint(w,path,view);revision=view->revision;redraw=false;}
        int k=dq_pollkey();if(k==DQK_NONE){task_sleep(10);continue;}redraw=true;
        if(k==CTRL('q')){if(!w->b.modified||w->confirm)run=false;else{w->confirm=true;ksnprintf(w->status,sizeof w->status,"未保存です。もう一度 Ctrl-Q で終了 / Ctrl-S で保存");}continue;}
        w->confirm=false;w->status[0]=0;
        switch(k) {
        case CTRL('s'):save(w,path);break;
        case CTRL('z'):scheme_buffer_undo(&w->b);break;
        case CTRL('t'):w->vertical=!w->vertical;w->top=0;ksnprintf(w->status,sizeof w->status,w->vertical?"縦書きに切り替えました":"横書きに切り替えました");kprintf("write: layout %s\n",w->vertical?"vertical":"horizontal");break;
        case DQK_F3:resize_font(w,-1);break;
        case DQK_F4:resize_font(w,1);break;
        case DQK_LEFT:if(w->vertical)move_across_lines(w,1);else w->b.cursor=scheme_buffer_prev(&w->b,w->b.cursor);break;
        case DQK_RIGHT:if(w->vertical)move_across_lines(w,-1);else w->b.cursor=scheme_buffer_next(&w->b,w->b.cursor);break;
        case DQK_UP:if(w->vertical)move_inside_column(w,-1);else move_across_lines(w,-1);break;
        case DQK_DOWN:if(w->vertical)move_inside_column(w,1);else move_across_lines(w,1);break;
        case DQK_PGUP:move_across_lines(w,w->vertical?-v_columns(w):-h_rows(w));break;
        case DQK_PGDN:move_across_lines(w,w->vertical?v_columns(w):h_rows(w));break;
        case DQK_HOME:
            if(w->vertical)w->b.cursor=w->starts[cursor_line(w)];
            else while(w->b.cursor&&w->b.text[w->b.cursor-1]!='\n')w->b.cursor=scheme_buffer_prev(&w->b,w->b.cursor);
            break;
        case DQK_END:
            if(w->vertical){int c=cursor_line(w);size_t end=w->starts[c+1];if(c+1<w->lines&&end>w->starts[c])end=scheme_buffer_prev(&w->b,end);w->b.cursor=end;}
            else while(w->b.cursor<w->b.length&&w->b.text[w->b.cursor]!='\n')w->b.cursor=scheme_buffer_next(&w->b,w->b.cursor);
            break;
        case DQK_BACKSPACE:scheme_buffer_delete(&w->b,true);break;
        case DQK_DEL:scheme_buffer_delete(&w->b,false);break;
        default:{char bytes[4];size_t n=1;bytes[0]=(char)k;
            if(k==DQK_ENTER)bytes[0]='\n';
            else if(k>=0xc2&&k<=0xf4){n=k>=0xf0?4:k>=0xe0?3:2;for(size_t i=1;i<n;i++)bytes[i]=(char)dq_getkey();}
            else if(k!='\t'&&(k<32||k>=127))break;
            if(!scheme_buffer_insert(&w->b,bytes,n))
                ksnprintf(w->status,sizeof w->status,"入力できません（最大16 KB）");
            break;
        }}
    }
    ime_set_app_view(false);ime_clear_anchor();fbcon_pause(false);display_release_claim(w);kfree(view);
    bizime_set_font(false,24);if(w->outline)otfont_end();else bizud_end();
    kfree(w->frame);kfree(w);return ok;
}
#endif
