#include "../../user/basic/basic.h"
#include "../../user/basic/graphics.h"
#include <m5os/display.h>
#include <m5os/arch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ready=1, paused, bypass;
static unsigned clears, points, lines, labels, pauses, resumes, slept, targets;
static int min_point_y, max_point_y;
static int last[5]; static char label[128]; static uint16_t background;
static char output[8192]; static size_t used;
uint32_t arch_irq_disable(void) { return 1; }
void arch_irq_restore(uint32_t irq) { assert(irq==1); }
bool display_is_ready(void) { return ready; }
bool fbcon_is_paused(void) { return paused; }
void fbcon_pause(bool p) { paused=p; if (p) pauses++; else resumes++; }
void ime_bypass(bool b) { bypass=b; }
void task_sleep(uint32_t ms) { assert(ms<=100); slept+=ms; }
void display_set_target(uint16_t *p) { assert(!p); targets++; }
void display_clear(uint16_t color) { clears++; background=color; }
void display_fill_rect(int x,int y,int w,int h,uint16_t color) {
    assert(paused && w==1 && h==1 && x>=0 && x<DISPLAY_WIDTH && y>=0 && y<DISPLAY_HEIGHT);
    if (y<min_point_y) min_point_y=y;
    if (y>max_point_y) max_point_y=y;
    points++; last[0]=x;last[1]=y;last[2]=color;
}
void display_draw_line(int x,int y,int x2,int y2,uint16_t color) {
    assert(paused); lines++; last[0]=x;last[1]=y;last[2]=x2;last[3]=y2;last[4]=color;
}
void display_draw_text(int x,int y,const char *s,uint16_t fg,uint16_t bg,int scale) {
    assert(paused && scale==2 && bg==background); labels++; strcpy(label,s); last[0]=x;last[1]=y;last[2]=fg;
}
typedef struct { basic_graphics g; unsigned polls, break_after; int input_status; } context;
static void write_output(const char *p,size_t n,void *opaque) {
    (void)opaque; assert(used+n<sizeof output); memcpy(output+used,p,n); used+=n;output[used]=0;
}
static int cancel(void *opaque) { context *c=opaque; return c->break_after && ++c->polls>=c->break_after; }
static const char *graphics(void *opaque,int op,const int32_t a[5],const char *text,size_t len) {
    context *c=opaque; return basic_graphics_command(&c->g,op,a,text,len,cancel,c);
}
static int input(void *opaque,char *buf,size_t cap) {
    context *c=opaque; assert(!c->g.active && !paused && !bypass); snprintf(buf,cap,"42"); return c->input_status;
}
static void reset(context *c,basic_session *s) {
    assert(!c->g.active && !paused && !bypass);
    clears=points=lines=labels=pauses=resumes=slept=targets=0;
    min_point_y=DISPLAY_HEIGHT;max_point_y=-1; used=0;output[0]=0;
    memset(c,0,sizeof *c); basic_session_init(s,write_output,cancel,c); s->graphics=graphics; s->input=input;
}
static int run(basic_session *s,const char *source) {
    unsigned at; assert(!basic_program_parse(&s->source,source,strlen(source),&at));
    int result=basic_session_run(s);
    assert(!paused && !bypass); return result;
}
int main(void) {
    context c={0}; basic_session *s=malloc(sizeof *s); assert(s); reset(&c,s);
    assert(!display_try_claim(NULL));
    static int wasm_owner, other;
    assert(display_try_claim(&wasm_owner)); assert(display_try_claim(&wasm_owner));
    assert(!display_try_claim(&other)); display_release_claim(&other); assert(!display_try_claim(&other));
    assert(run(s,"10 SCREEN 1\n")==-1 && strstr(output,"display busy") && !pauses && !resumes);
    display_release_claim(&wasm_owner);
    reset(&c,s); ready=0; assert(run(s,"10 SCREEN 1\n")==-1 && strstr(output,"unavailable")); ready=1;
    /* Do not unpause an unrelated legacy full-screen caller. */
    paused=1;
    unsigned at; assert(!basic_program_parse(&s->source,"10 SCREEN 1\n",12,&at));
    assert(basic_session_run(s)==-1 && paused && !c.g.active); paused=0;
    reset(&c,s);
    assert(!run(s,"10 SCREEN 1\n20 CLS 31\n30 PSET 0,0,65535\n40 LINE 0,0,1279,719,63488\n50 TEXT 20,30,\"MeOS\",65535\n60 WAIT 25\n"));
    assert(points==1 && lines==1 && labels==1 && !strcmp(label,"MeOS"));
    assert(pauses==1 && resumes==1 && slept==125 && clears==2 && targets==2);
    assert(display_try_claim(&wasm_owner)); display_release_claim(&wasm_owner);
    const char *bad[]={
        "10 SCREEN 2\n", "10 WAIT -1\n", "10 WAIT 60001\n",
        "10 SCREEN 1\n20 CLS 65536\n", "10 SCREEN 1\n20 PSET -1,0,1\n",
        "10 SCREEN 1\n20 PSET 1280,0,1\n", "10 SCREEN 1\n20 LINE 0,0,2147483647,0,1\n",
        "10 SCREEN 1\n20 TEXT 0,0,3,1\n", "10 SCREEN 1\n20 PSET 1,2,3,4\n",
        "10 SCREEN 1\n20 TEXT 0,0,\"日本語\",1\n", "10 PSET 1,2,3\n",
        "10 SCREEN 1\n20 PRINT 1/0\n"
    };
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
        reset(&c,s); assert(run(s,bad[i])==-1); assert(!c.g.active);
        assert(display_try_claim(&other)); display_release_claim(&other);
    }
    reset(&c,s); c.break_after=10;
    assert(run(s,"10 SCREEN 1\n20 WAIT 60000\n30 PSET 1,2,3\n")==-2);
    assert(strstr(output,"Break") && !points && resumes==1 && slept<300);
    reset(&c,s); c.break_after=4;
    assert(run(s,"10 SCREEN 1\n20 GOTO 20\n")==-2 && resumes==1);
    reset(&c,s);
    assert(run(s,"10 SCREEN 1\n20 GOTO 20\n")==-2 && resumes==1 && strstr(output,"execution limit"));
    reset(&c,s);
    assert(!run(s,"10 SCREEN 1\n20 INPUT A\n30 PRINT A\n"));
    assert(strstr(output,"42") && resumes==1);
    reset(&c,s); s->input=NULL;
    assert(run(s,"10 SCREEN 1\n20 INPUT A\n")==-1 && resumes==1);
    reset(&c,s); c.input_status=-1;
    assert(run(s,"10 SCREEN 1\n20 INPUT A\n")==-2 && resumes==1);
    reset(&c,s);
    assert(!run(s,"10 SCREEN 1\n20 SCREEN 0\n30 SCREEN 1\n40 SCREEN 0\n"));
    assert(pauses==2 && resumes==2);
    reset(&c,s);
    assert(!run(s,"10 IF 0 THEN SCREEN 1 ELSE WAIT 0\n")); assert(!pauses && !resumes);
    /* Adapter validates ranges even if called outside the parser. */
    int32_t args[5]={1}; assert(!graphics(&c,UBASIC_SCREEN,args,NULL,0));
    args[0]=0; args[1]=0; args[2]=-1; assert(graphics(&c,UBASIC_PSET,args,NULL,0));
    args[0]=0; assert(!graphics(&c,UBASIC_SCREEN,args,NULL,0));
    reset(&c,s);
    FILE *fp=fopen("user/basic/examples/graphics.bas","rb"); assert(fp);
    char sample[1024]; size_t n=fread(sample,1,sizeof sample-1,fp); assert(feof(fp)&&!ferror(fp)); fclose(fp); sample[n]=0;
    assert(!run(s,sample)); assert(lines==201 && labels==1 && resumes==1);
    reset(&c,s);
    fp=fopen("user/basic/examples/sine_plot.bas","rb");assert(fp);
    n=fread(sample,1,sizeof sample-1,fp);assert(feof(fp)&&!ferror(fp));fclose(fp);sample[n]=0;
    assert(!run(s,sample));assert(points==126 && lines==1 && labels==1 && resumes==1);
    assert(min_point_y==80 && max_point_y==279);
    reset(&c,s);c.break_after=15;
    assert(run(s,sample)==-2 && resumes==1 && !c.g.active);
    free(s); puts("BASIC graphics/cleanup/lease tests PASS");
}
