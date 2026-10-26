#include <m5os/board_config.h>
#include <m5os/quake.h>
#include <m5os/keyboard_raw.h>
#include <m5os/audio.h>
#if M5OS_QUAKE
#include <m5os/app.h>
#include <m5os/task.h>
#include <m5os/arch.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/display.h>
#include <m5os/hal_time.h>
#include <m5os/ime.h>
#include <m5os/vfs.h>
#include "../../drivers/lcd/fbcon.h"
#include "../../drivers/tab5kbd/tab5touch.h"
#include "runtime.h"
#include "input.h"
#include "layout.h"
#include "../../components/quakeengine/profile.h"
#define QUAKE_COMPAT_DECLARATIONS_ONLY
#include "../../components/quakeengine/compat.h"
#include "../../third_party/quakegeneric/source/quakedef.h"
#include "../../third_party/quakegeneric/source/quakegeneric.h"
#include <string.h>
uint32_t quake_owner;
char quake_basedir[96]="/sd";
extern char _quake_data_start[],_quake_data_end[],_quake_bss_start[],_quake_bss_end[],_quake_common_start[],_quake_common_end[];
static void *initial;
static jmp_buf escape;
static bool pending,screen,controls_only,benchmark_start,smooth_requested,ppa_test;
static bool smooth_active,silent_requested;
static bool dma_requested,dma_active,dma_test,blit_bench;
static bool visual_test;
static char selected_map[32]="start";
static uint16_t palette[256];
static uint8_t *previous_frame;
static bool palette_dirty;
static unsigned frames;
static uint64_t began,profile_at;
static unsigned profile_frames,native_events;
static bool native_ready;
static bool sampled,capture_next;
static bool legacy_wait=false;
static uint64_t prof_host,prof_draw,prof_copy,prof_flush,prof_input,prof_wait,prof_yield;
static unsigned profile_scan;
static uint32_t cpu_pid[TASK_MAX];
static uint64_t cpu_previous[TASK_MAX];
static bool cpu_baseline;
static void profile_cpu(unsigned frames_in_interval){
    task_runtime_info_t tasks[TASK_MAX];int count=task_runtime_snapshot(tasks,TASK_MAX);
    uint64_t own=0,other=0;
    for(int i=0;i<count;i++){
        uint64_t delta=0;
        for(unsigned j=0;j<TASK_MAX;j++)if(cpu_pid[j]==tasks[i].id){delta=tasks[i].runtime_us-cpu_previous[j];break;}
        if(tasks[i].id==task_current()->id)own=delta;else other+=delta;
        if(cpu_baseline&&tasks[i].id!=task_current()->id&&delta/frames_in_interval>=100)
            kprintf("quake cpu task: %s %u us/frame\n",tasks[i].name,(unsigned)(delta/frames_in_interval));
    }
    if(cpu_baseline)kprintf("quake cpu: own %u other-live %u us/frame (scheduled time, includes IRQs)\n",
        (unsigned)(own/frames_in_interval),(unsigned)(other/frames_in_interval));
    memset(cpu_pid,0,sizeof cpu_pid);
    for(int i=0;i<count;i++){cpu_pid[i]=tasks[i].id;cpu_previous[i]=tasks[i].runtime_us;}
    cpu_baseline=true;
}
static void profile_reset(void){
    prof_host=prof_draw=prof_copy=prof_flush=prof_input=prof_wait=prof_yield=0;
    profile_at=hal_time_now_us();profile_frames=frames;profile_scan=display_scanout_count();
}
static void profile_report(void){
    unsigned n=frames-profile_frames;if(n<100)return;
    profile_cpu(n);
    uint64_t elapsed=hal_time_now_us()-profile_at;
    unsigned scan=display_scanout_count()-profile_scan;
    unsigned fps=(unsigned)((uint64_t)n*10000000/elapsed);
    unsigned hz=(unsigned)((uint64_t)scan*10000000/elapsed);
    uint64_t other=prof_host>prof_draw+prof_input+prof_yield?prof_host-prof_draw-prof_input-prof_yield:0;
    kprintf("quake perf: %s %u.%u fps LCD %u.%u Hz, host %u draw %u (copy %u flush %u) input %u wait %u other %u us/frame\n",
        legacy_wait?"baseline":dma_active?"dma-exact":smooth_active?"ppa-smooth":"no-extra-wait",fps/10,fps%10,hz/10,hz%10,
        (unsigned)(prof_host/n),(unsigned)(prof_draw/n),(unsigned)(prof_copy/n),(unsigned)(prof_flush/n),
        (unsigned)(prof_input/n),(unsigned)(prof_wait/n),(unsigned)(other/n));
    kprintf("quake keyboard: %s, %u native transitions\n",native_ready?"ready":"waiting",native_events);
    quake_profile_stats p=quake_profile_take();unsigned pn=p.completed_frames?p.completed_frames:n;
    kprintf("quake phases: cmd %u server %u client %u render %u audio %u us/frame (%u completed)\n",
        (unsigned)(p.phase_us[QP_COMMANDS]/pn),(unsigned)(p.phase_us[QP_SERVER]/pn),
        (unsigned)(p.phase_us[QP_CLIENT]/pn),(unsigned)(p.phase_us[QP_RENDER]/pn),
        (unsigned)(p.phase_us[QP_AUDIO]/pn),p.completed_frames);
#ifdef MEOS_QUAKE_DETAIL_PROFILE
    kprintf("quake detail outer: setup %u edges %u entities %u viewmodel %u particles %u warp %u us/frame\n",
        (unsigned)(p.detail_us[QD_SETUP]/pn),(unsigned)(p.detail_us[QD_EDGES]/pn),
        (unsigned)(p.detail_us[QD_ENTITIES]/pn),(unsigned)(p.detail_us[QD_VIEWMODEL]/pn),
        (unsigned)(p.detail_us[QD_PARTICLES]/pn),(unsigned)(p.detail_us[QD_WARP]/pn));
    unsigned detail_calls=0;for(unsigned i=0;i<QD_COUNT;i++)detail_calls+=p.detail_calls[i];
    kprintf("quake detail nested: cache %u build %u light %u spans %u z %u sky %u water %u us/frame; %u timer pairs\n",
        (unsigned)(p.detail_us[QD_CACHE]/pn),(unsigned)(p.detail_us[QD_BUILD]/pn),
        (unsigned)(p.detail_us[QD_LIGHT]/pn),(unsigned)(p.detail_us[QD_SPANS]/pn),
        (unsigned)(p.detail_us[QD_ZSPANS]/pn),(unsigned)(p.detail_us[QD_SKY]/pn),
        (unsigned)(p.detail_us[QD_WATER]/pn),detail_calls);
#endif
    kprintf("quake io: %u bytes, open %u/%u read %u/%u seek %u/%u calls/us\n",
        (unsigned)p.read_bytes,p.io_calls[QIO_OPEN],(unsigned)p.io_us[QIO_OPEN],
        p.io_calls[QIO_READ],(unsigned)p.io_us[QIO_READ],p.io_calls[QIO_SEEK],(unsigned)p.io_us[QIO_SEEK]);
    kprintf("quake cache: hit %u rebuild %u alloc %u evict %u wrap %u thrash %u; pixels %u bytes %u\n",
        (unsigned)p.cache[QC_HIT],(unsigned)p.cache[QC_REBUILD],(unsigned)p.cache[QC_ALLOC],
        (unsigned)p.cache[QC_EVICT],(unsigned)p.cache[QC_WRAP],(unsigned)p.cache[QC_THRASH],
        (unsigned)p.cache[QC_PIXELS],(unsigned)p.cache[QC_BYTES]);
    kprintf("quake cache reasons: dynamic %u texture %u lightstyle %u (overlap; existing entries only)\n",
        (unsigned)p.cache[QC_DYNAMIC],(unsigned)p.cache[QC_TEXTURE],(unsigned)p.cache[QC_LIGHTSTYLE]);
    task_stack_info_t stack;if(!task_stack_inspect(task_current()->id,&stack))
        kprintf("quake stack: %u/%u bytes, guard %s\n",(unsigned)stack.used,(unsigned)stack.size,stack.guard_ok?"OK":"BAD");
    profile_reset();
}
static void reset_engine(void) {
    memset(_quake_bss_start,0,(size_t)(_quake_bss_end-_quake_bss_start));
    memset(_quake_common_start,0,(size_t)(_quake_common_end-_quake_common_start));
    memcpy(_quake_data_start,initial,(size_t)(_quake_data_end-_quake_data_start));
}
void quake_exit(int code) { longjmp(escape,code?2:1); }
int quake_app_cleanup(uint32_t id) {
    if(id!=quake_owner||!id)return 0;
    extern void quake_sound_cleanup(void);quake_sound_cleanup();
    display_dma_end(id);dma_active=false;
    display_smooth_end(id);smooth_active=false;
    quake_io_close_all();
    keyboard_raw_release(&quake_owner);
    if(screen) { display_set_target(NULL);ime_bypass(false);kconsole_set_focus(0);fbcon_pause(false); }
    display_release_claim(&quake_owner);screen=false;
    previous_frame=NULL;
    if(initial)reset_engine();
    quake_owner=0;return 0;
}
void QG_Init(void) {}
void QG_Quit(void) { quake_exit(0); }
void QG_SetPalette(unsigned char p[768]) { for(int i=0;i<256;i++){uint16_t color=RGB565(p[i*3],p[i*3+1],p[i*3+2]);if(color!=palette[i])palette_dirty=true;palette[i]=color;} }
static uint32_t paint_mask;
static void paint_buttons(uint32_t mask,bool all){
    for(unsigned i=0;i<QUAKE_BUTTON_COUNT;i++){
        if(!all&& !((mask^paint_mask)&(1u<<i)))continue;
        const quake_button *b=&quake_buttons[i];uint16_t color=i==QB_FIRE?RGB565(140,60,25):i==QB_JUMP?RGB565(30,90,60):RGB565(35,55,75);
        if(i==QB_EXIT)color=RGB565(100,35,35);
        if(mask&(1u<<i))color=RGB565(90,135,170);
        display_fill_rect(b->x,b->y,b->w,b->h,color);display_draw_rect(b->x,b->y,b->w,b->h,RGB565(90,115,135));
        int scale=strlen(b->label)==1?3:2;int tx=b->x+(b->w-(int)strlen(b->label)*8*scale)/2;
        display_draw_text(tx,b->y+(b->h-8*scale)/2,b->label,COLOR_WHITE,color,scale);
    }
    paint_mask=mask;
}
static void capture(const uint8_t *pixels) {
    static const char digits[]="0123456789abcdef";
    char line[QV_WIDTH*2+1];kprintf("QSHOT %u %u %u\n",QV_WIDTH,QV_HEIGHT,frames);
    for(unsigned row=0;row<16;row++) {
        for(unsigned col=0;col<16;col++) { unsigned n=palette[row*16+col];for(unsigned k=0;k<4;k++)line[col*4+k]=digits[(n>>(12-4*k))&15]; }
        line[64]=0;kprintf("QPAL %u %s\n",row,line);
    }
    for(unsigned row=0;row<QV_HEIGHT;row++) {
        for(unsigned col=0;col<QV_WIDTH;col++){unsigned n=pixels[row*QV_WIDTH+col];line[col*2]=digits[n>>4];line[col*2+1]=digits[n&15];}
        line[QV_WIDTH*2]=0;kprintf("QROW %u %s\n",row,line);
    }
    kputs("QEND\n");
}
void QG_DrawFrame(void *pixels) {
    uint64_t t=hal_time_now_us();
    bool accelerated=dma_active&&display_dma_present(pixels,previous_frame,palette,palette_dirty||!frames);
    if(dma_active&&!accelerated){display_dma_end(quake_owner);dma_active=false;palette_dirty=true;}
    if(!accelerated&&smooth_active)accelerated=display_smooth_present(pixels,palette);
    if(smooth_active&&!accelerated){display_smooth_end(quake_owner);smooth_active=false;palette_dirty=true;}
    if(accelerated){}
    else if(previous_frame)display_draw_indexed_viewport_delta(QV_X,pixels,previous_frame,palette,palette_dirty||!frames);
    else display_draw_indexed_bitmap_resized(QV_X,QV_Y,QV_WIDTH,QV_HEIGHT,pixels,palette,QV_OUT_WIDTH,QV_OUT_HEIGHT);
    palette_dirty=false;
    prof_draw+=hal_time_now_us()-t;
    uint32_t copy,flush;display_last_present_times(&copy,&flush);prof_copy+=copy;prof_flush+=flush;
    frames++;sampled=false;
    if(capture_next){capture_next=false;capture(pixels);}
    if(legacy_wait){uint64_t t=hal_time_now_us();task_yield();uint64_t dt=hal_time_now_us()-t;prof_wait+=dt;prof_yield+=dt;}

}
static void sample_inputs(void) {
    uint64_t input_start=hal_time_now_us();
    uint32_t now=(uint32_t)(hal_time_now_us()/1000);
    int ch;for(unsigned i=0;i<64 && (ch=kconsole_getc())>=0;i++)quake_input_byte(ch,(int)key_dest,now);
    keyboard_raw_state keys;keyboard_raw_read(&quake_owner,&keys);
    native_ready=keys.ready;native_events+=keys.count;
    tab5_touch_point_t points[10];unsigned count=tab5_touch_read_points(points,10);
    quake_input_sample((int)key_dest,&keys,points,count,now);
    paint_buttons(quake_input_touch_buttons(),false);
    int action=quake_input_action();if(action==1)quake_exit(0);if(action==2)capture_next=true;
    prof_input+=hal_time_now_us()-input_start;
    if(action==3){legacy_wait=!legacy_wait;profile_reset();kprintf("quake perf: mode %s\n",legacy_wait?"baseline":"no-extra-wait");}
}
int QG_GetKey(int *down,int *key) {
    if(!sampled){sample_inputs();sampled=true;}
    int n=quake_input_next(down,key);
    if(quake_input_action()==1)quake_exit(0);
    return n;
}
void QG_GetMouseMove(int *x,int *y){quake_input_mouse(x,y);}
void QG_GetJoyAxes(float *axes){for(int i=0;i<QUAKEGENERIC_JOY_MAX_AXES;i++)axes[i]=0;}
static void transfer_benchmark(void){
    uint8_t *pixels=quake_malloc(QV_PIXELS),*old=quake_malloc(QV_PIXELS);uint16_t colors[256];
    for(unsigned i=0;i<256;i++)colors[i]=(uint16_t)(i*239);
    bool available=dma_active;
    for(unsigned backend=0;backend<(available?2u:1u);backend++){
    dma_active=available&&backend==1;
    for(unsigned mode=0;mode<3;mode++){
        for(unsigned i=0;i<QV_PIXELS;i++)pixels[i]=(uint8_t)(i*13);
        if(dma_active){if(!display_dma_present(pixels,old,colors,true))quake_exit(1);}
        else display_draw_indexed_viewport_delta(QV_X,pixels,old,colors,true);
        uint64_t wall=0,cpu=0;
        for(unsigned f=0;f<30;f++){
            if(mode)for(unsigned y=mode==2?24*QV_HEIGHT/200:0;y<(mode==2?152*QV_HEIGHT/200:QV_HEIGHT);y++)
                for(unsigned x=0;x<QV_WIDTH;x++)pixels[y*QV_WIDTH+x]=(uint8_t)(y*QV_WIDTH+x+f);
            uint64_t own=task_current_runtime_us(),at=hal_time_now_us();
            if(dma_active){if(!display_dma_present(pixels,old,colors,false))quake_exit(1);}
            else display_draw_indexed_viewport_delta(QV_X,pixels,old,colors,false);
            wall+=hal_time_now_us()-at;cpu+=task_current_runtime_us()-own;
        }
        kprintf("quake blitbench: %s %s wall %u own %u us/frame checksum %08x\n",
            dma_active?"dma":"cpu",mode==0?"static":mode==1?"full":"odd-band",
            (unsigned)(wall/30),(unsigned)(cpu/30),display_viewport_checksum());
    }
    }
    dma_active=available;
}
static void transfer_visual_test(void){
    uint8_t *pixels=quake_malloc(QV_PIXELS),*old=quake_malloc(QV_PIXELS);
    const uint16_t colors[10]={0x0000,0xffff,0xf800,0x07e0,0x001f,0xffe0,0x07ff,0xf81f,0x8410,0x4208};
    uint16_t palette_test[256]={0};memcpy(palette_test,colors,sizeof colors);
    bool available=dma_active,use_dma=available,animate=false,redraw=true;unsigned phase=0;
    kputs("quake visual: SPACE switches CPU/DMA; A moves bar/pause; Ctrl-C or EXIT closes. Static grid is intentional.\n");
    for(;;){
        sample_inputs();int down,key;
        while(quake_input_next(&down,&key))if(down){
            if(key==' '&&available){use_dma=!use_dma;redraw=true;}
            if(key=='a'||key=='A'){animate=!animate;redraw=true;}
        }
        if(redraw||animate){
            for(unsigned y=0;y<QV_HEIGHT;y++)for(unsigned x=0;x<QV_WIDTH;x++){
                unsigned sx=x*320/QV_WIDTH,sy=y*200/QV_HEIGHT;
                uint8_t color=sy<140?(uint8_t)(2+sx/40%6):sy<170?(uint8_t)(8+(sx/10%2)):(uint8_t)((sx/8+sy/8)&1);
                if(sx%40==0||sy%20==0)color=1;
                if(sx<2||sx>=318||sy<2||sy>=198)color=1;
                if((sx<12&&sy<12)||(sx>=308&&sy>=188))color=2;
                if((sx>=308&&sy<12)||(sx<12&&sy>=188))color=4;
                if(animate&&sx>=phase%312&&sx<phase%312+8)color=0;
                pixels[y*QV_WIDTH+x]=color;
            }
            display_fill_rect(0,40,QV_X,150,COLOR_BLACK);
            display_draw_text(8,48,use_dma?"DMA EXACT":"CPU REF",COLOR_WHITE,COLOR_BLACK,1);
            display_draw_text(8,72,animate?"MOVING":"STATIC HOLD",COLOR_WHITE,COLOR_BLACK,1);
            display_draw_text(8,100,"SPACE: switch",COLOR_WHITE,COLOR_BLACK,1);
            display_draw_text(8,124,"A: move/pause",COLOR_WHITE,COLOR_BLACK,1);
            if(use_dma){if(!display_dma_present(pixels,old,palette_test,true)){kputs("quake visual: DMA transfer failed\n");quake_exit(1);}}
            else display_draw_indexed_viewport_delta(QV_X,pixels,old,palette_test,true);
            if(redraw||!animate||phase%60==0){
                int bx,by;uint32_t bad=display_verify_viewport(pixels,palette_test,&bx,&by);char message[64];
                ksnprintf(message,sizeof message,"MEM %s %u",bad?"FAIL":"PASS",bad);
                display_fill_rect(QV_X+QV_OUT_WIDTH,40,QV_X,100,COLOR_BLACK);display_draw_text(QV_X+QV_OUT_WIDTH+8,48,message,bad?RGB565(255,60,60):RGB565(80,255,100),COLOR_BLACK,1);
                display_draw_text(QV_X+QV_OUT_WIDTH+8,76,"MEMORY ONLY",COLOR_WHITE,COLOR_BLACK,1);
                display_draw_text(QV_X+QV_OUT_WIDTH+8,100,"CHECK SCREEN",COLOR_WHITE,COLOR_BLACK,1);
                kprintf("quake visual: %s %s mismatches %u first %d,%d checksum %08x\n",use_dma?"dma":"cpu",animate?"moving":"static",bad,bx,by,display_viewport_checksum());
            }
            redraw=false;if(animate)phase++;
        }
        task_sleep(animate?20:10);
    }
}
void quake_main(void *arg) {
    (void)arg;pending=false;
    if(!display_is_ready()||fbcon_is_paused()){kputs("quake: display busy/unavailable\n");return;}
    uint32_t irq=arch_irq_disable();
    if(quake_owner){arch_irq_restore(irq);return;}
    quake_owner=task_current()->app->app_id;
    arch_irq_restore(irq);
    if(!display_try_claim(&quake_owner)){quake_owner=0;return;}
    size_t n=(size_t)(_quake_data_end-_quake_data_start);
    if(!n||n>256u*1024u||(uintptr_t)_quake_bss_end<=(uintptr_t)_quake_bss_start){kputs("quake: invalid linker state\n");return;}
    if(!initial){initial=kmalloc_psram(n);if(!initial)return;memcpy(initial,_quake_data_start,n);}
    reset_engine();quake_profile_reset();cpu_baseline=false;memset(cpu_pid,0,sizeof cpu_pid);
    quake_input_reset();native_events=0;native_ready=false;sampled=false;capture_next=false;
    if(!keyboard_raw_claim(&quake_owner)){kputs("quake: native keyboard is busy\n");return;}
    kconsole_set_focus(task_current()->id);ime_bypass(true);fbcon_pause(true);screen=true;task_sleep(60);
    display_set_target(NULL);display_clear(COLOR_BLACK);
    display_fill_rect(0,0,QV_X,720,RGB565(15,22,30));display_fill_rect(QV_X+QV_OUT_WIDTH,0,QV_X,720,RGB565(15,22,30));
    display_draw_text(QV_X/2-32,284,"MOVE",COLOR_WHITE,RGB565(15,22,30),2);
    display_draw_text(QV_X+QV_OUT_WIDTH+QV_X/2-32,188,"LOOK",COLOR_WHITE,RGB565(15,22,30),2);
    paint_mask=0;paint_buttons(0,true);
    frames=0;began=hal_time_now_us();
    if(!setjmp(escape)) {
        smooth_active=smooth_requested&&display_smooth_begin(quake_owner);
        dma_active=dma_requested&&display_dma_begin(quake_owner);
        if(dma_requested&&!dma_active)kputs("quake: exact DMA unavailable; CPU retained\n");
        if(ppa_test||dma_test)quake_exit(0);
        if(visual_test)transfer_visual_test();
        if(blit_bench){transfer_benchmark();quake_exit(0);}
        previous_frame=controls_only?NULL:quake_malloc(QV_PIXELS);palette_dirty=true;
        if(controls_only) {
            display_draw_text(240,100,"QUAKE CONTROLS",COLOR_WHITE,COLOR_BLACK,3);
            display_draw_text(240,160,"WASD MOVE / ARROWS TURN",COLOR_WHITE,COLOR_BLACK,2);
            display_draw_text(240,200,"SPACE JUMP / CTRL OR F FIRE",COLOR_WHITE,COLOR_BLACK,2);
            display_draw_text(240,240,"DRAG VIEW TO LOOK / E WEAPON",COLOR_WHITE,COLOR_BLACK,2);
            display_draw_text(240,280,"EXIT BUTTON OR CTRL-C TO CLOSE",COLOR_WHITE,COLOR_BLACK,2);
            kputs("quake controls: input preview; no game data needed\n");
            bool ready=false;
            for(;;) {
                sample_inputs();int down,key;
                while(quake_input_next(&down,&key)) {
                    char line[64];ksnprintf(line,sizeof line,"KEY %d %s",key,down?"DOWN":"UP");
                    display_fill_rect(240,400,700,32,COLOR_BLACK);
                    display_draw_text(240,400,line,COLOR_WHITE,COLOR_BLACK,2);
                    kprintf("quake controls: %s\n",line);
                }
                if(native_ready!=ready) {
                    ready=native_ready;
                    display_fill_rect(240,340,700,32,COLOR_BLACK);
                    display_draw_text(240,340,ready?"KEYBOARD READY":"KEYBOARD WAITING",COLOR_WHITE,COLOR_BLACK,2);
                    kprintf("quake controls: keyboard %s\n",ready?"ready":"waiting");
                }
                int dx,dy;quake_input_mouse(&dx,&dy);
                task_sleep(20);
            }
        }
        quakeparms_t parms={0};parms.memsize=12*1024*1024;parms.membase=quake_malloc(parms.memsize);parms.basedir=quake_basedir;
        char *args[11]={"quake","-basedir",quake_basedir,"-nocdaudio","-surfcachesize","600"};int ac=6;
        if(silent_requested)args[ac++]="-nosound";
        if(benchmark_start){args[ac++]="+map";args[ac++]=selected_map;}
        args[ac]=NULL;
        COM_InitArgv(ac,args);parms.argc=com_argc;parms.argv=com_argv;
        uint64_t load_at=hal_time_now_us();bool playable_reported=false;
#ifdef MEOS_QUAKE_DETAIL_PROFILE
        uint64_t clock_at=hal_time_now_us();for(unsigned i=0;i<1024;i++)(void)quake_profile_clock();
        kprintf("quake detail: enabled (inclusive wall time), clock read %u ns avg incl loop/IRQs\n",
            (unsigned)((hal_time_now_us()-clock_at)*1000/1024));
#endif
        Host_Init(&parms);
        Cbuf_AddText("bind w +forward\nbind s +back\nbind a +moveleft\nbind d +moveright\nbind f +attack\nbind e \"impulse 10\"\nbind / \"impulse 10\"\nbind UPARROW +forward\nbind DOWNARROW +back\nbind LEFTARROW +left\nbind RIGHTARROW +right\n+mlook\n");
        frames=0;profile_frames=0;began=profile_at=hal_time_now_us();
        kprintf("quake: %ux%u -> %ux%u at %u,%u; Host_Init complete\n",QV_WIDTH,QV_HEIGHT,QV_OUT_WIDTH,QV_OUT_HEIGHT,QV_X,QV_Y);
#if M5OS_QUAKE_HIGHRES && M5OS_QUAKE_ZPAD
        kprintf("quake: experimental Z pitch %u pixels (+%u bytes PSRAM)\n",QV_Z_PITCH,
            (unsigned)((QV_Z_PITCH-QV_WIDTH)*QV_HEIGHT*2));
#endif
        profile_reset();uint64_t last=hal_time_now_us();
        for(;;){
            uint64_t now=hal_time_now_us();unsigned before=frames;
            Host_Frame((now-last)/1000000.0);last=now;
            if(!playable_reported&&frames&&cls.signon==SIGNONS){
                playable_reported=true;kprintf("quake load: %u ms to playable signon\n",(unsigned)((hal_time_now_us()-load_at)/1000));
            }
            prof_host+=hal_time_now_us()-now;
            uint64_t wait_start=hal_time_now_us();
            if(legacy_wait||frames==before)task_sleep(1);
            prof_wait+=hal_time_now_us()-wait_start;
            profile_report();
        }
    }
    kprintf("quake: stopped after %u frames\n",frames);
}
extern int quake_receive(int argc,char **argv);
extern int quake_verify(void);
int quake_command(int argc,char **argv) {
    if(quake_owner||pending)return 1;
    if(argc>=2&&!strcmp(argv[1],"receive"))return quake_receive(argc,argv);
    if(argc==2&&!strcmp(argv[1],"verify"))return quake_verify();
    smooth_requested=ppa_test=benchmark_start=controls_only=silent_requested=false;
    dma_requested=dma_test=blit_bench=false;
    visual_test=false;
    const char *base=quake_storage_base();bool map_set=false;strcpy(selected_map,"start");
    for(int i=1;i<argc;i++){
        const char *arg=argv[i];
        if(!strcmp(arg,"--silent")){silent_requested=true;continue;}
        if(!strcmp(arg,"dma")){dma_requested=true;benchmark_start=true;continue;}
        if(!strcmp(arg,"dmatest")){dma_requested=dma_test=controls_only=true;continue;}
        if(!strcmp(arg,"blitbench")){blit_bench=controls_only=true;continue;}
        if(!strcmp(arg,"dmabench")){dma_requested=blit_bench=controls_only=true;continue;}
        if(!strcmp(arg,"dmaview")){dma_requested=visual_test=controls_only=true;continue;}
        if(!strcmp(arg,"smooth")){smooth_requested=true;benchmark_start=true;continue;}
        if(!strcmp(arg,"bench")){benchmark_start=true;continue;}
        if(!strcmp(arg,"controls")){controls_only=true;continue;}
        if(!strcmp(arg,"ppatest")){controls_only=ppa_test=smooth_requested=true;continue;}
        if(!strcmp(arg,"/sd")||!strcmp(arg,"/assets/quake")){base=arg;continue;}
        size_t n=strlen(arg);bool valid=n>0&&n<sizeof selected_map&&!map_set;
        for(size_t j=0;j<n;j++)if(!((arg[j]>='a'&&arg[j]<='z')||(arg[j]>='0'&&arg[j]<='9')||arg[j]=='_'))valid=false;
        if(!valid){kputs("usage: quake [dma|smooth|bench] [MAP] [--silent]; diagnostics: controls ppatest dmatest blitbench dmabench dmaview\n");return 1;}
        strcpy(selected_map,arg);map_set=true;benchmark_start=true;
    }
    if(M5OS_QUAKE_HIGHRES&&smooth_requested){kputs("quake: smooth is only available at 320x200\n");return 1;}
    if(dma_requested&&smooth_requested){kputs("quake: choose dma or smooth\n");return 1;}
    ksnprintf(quake_basedir,sizeof quake_basedir,"%s",base);
    char path[128];ksnprintf(path,sizeof path,"%s/id1/pak0.pak",base);
    vfs_info_t info;if(!controls_only&&vfs_stat(path,&info)<0){kprintf("quake: missing %s; use quake receive BYTES CRC32hex\n",path);return 1;}
    pending=true;uint32_t id;int rc=app_start("quake",&id);
    if(rc<0){pending=false;kprintf("quake: could not start app (%d)\n",rc);}
    return rc<0;
}
#else
void quake_main(void *arg) {(void)arg;}
int quake_command(int argc,char **argv){(void)argc;(void)argv;return -4;}
int quake_app_cleanup(uint32_t id){(void)id;return 0;}
#endif
