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
static void profile_reset(void){
    prof_host=prof_draw=prof_copy=prof_flush=prof_input=prof_wait=prof_yield=0;
    profile_at=hal_time_now_us();profile_frames=frames;profile_scan=display_scanout_count();
}
static void profile_report(void){
    unsigned n=frames-profile_frames;if(n<100)return;
    uint64_t elapsed=hal_time_now_us()-profile_at;
    unsigned scan=display_scanout_count()-profile_scan;
    unsigned fps=(unsigned)((uint64_t)n*10000000/elapsed);
    unsigned hz=(unsigned)((uint64_t)scan*10000000/elapsed);
    uint64_t other=prof_host>prof_draw+prof_input+prof_yield?prof_host-prof_draw-prof_input-prof_yield:0;
    kprintf("quake perf: %s %u.%u fps LCD %u.%u Hz, host %u draw %u (copy %u flush %u) input %u wait %u other %u us/frame\n",
        legacy_wait?"baseline":smooth_active?"ppa-smooth":"no-extra-wait",fps/10,fps%10,hz/10,hz%10,
        (unsigned)(prof_host/n),(unsigned)(prof_draw/n),(unsigned)(prof_copy/n),(unsigned)(prof_flush/n),
        (unsigned)(prof_input/n),(unsigned)(prof_wait/n),(unsigned)(other/n));
    kprintf("quake keyboard: %s, %u native transitions\n",native_ready?"ready":"waiting",native_events);
    quake_profile_stats p=quake_profile_take();unsigned pn=p.completed_frames?p.completed_frames:n;
    kprintf("quake phases: cmd %u server %u client %u render %u audio %u us/frame (%u completed)\n",
        (unsigned)(p.phase_us[QP_COMMANDS]/pn),(unsigned)(p.phase_us[QP_SERVER]/pn),
        (unsigned)(p.phase_us[QP_CLIENT]/pn),(unsigned)(p.phase_us[QP_RENDER]/pn),
        (unsigned)(p.phase_us[QP_AUDIO]/pn),p.completed_frames);
    kprintf("quake io: %u bytes, open %u/%u read %u/%u seek %u/%u calls/us\n",
        (unsigned)p.read_bytes,p.io_calls[QIO_OPEN],(unsigned)p.io_us[QIO_OPEN],
        p.io_calls[QIO_READ],(unsigned)p.io_us[QIO_READ],p.io_calls[QIO_SEEK],(unsigned)p.io_us[QIO_SEEK]);
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
    char line[641];kprintf("QSHOT 320 200 %u\n",frames);
    for(unsigned row=0;row<16;row++) {
        for(unsigned col=0;col<16;col++) { unsigned n=palette[row*16+col];for(unsigned k=0;k<4;k++)line[col*4+k]=digits[(n>>(12-4*k))&15]; }
        line[64]=0;kprintf("QPAL %u %s\n",row,line);
    }
    for(unsigned row=0;row<200;row++) {
        for(unsigned col=0;col<320;col++){unsigned n=pixels[row*320+col];line[col*2]=digits[n>>4];line[col*2+1]=digits[n&15];}
        line[640]=0;kprintf("QROW %u %s\n",row,line);
    }
    kputs("QEND\n");
}
void QG_DrawFrame(void *pixels) {
    uint64_t t=hal_time_now_us();
    bool accelerated=smooth_active&&display_smooth_present(pixels,palette);
    if(smooth_active&&!accelerated){display_smooth_end(quake_owner);smooth_active=false;palette_dirty=true;}
    if(accelerated){}
    else if(previous_frame)display_draw_indexed_viewport_delta(160,pixels,previous_frame,palette,palette_dirty||!frames);
    else display_draw_indexed_bitmap_resized(160,0,320,200,pixels,palette,960,720);
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
    reset_engine();quake_profile_reset();quake_input_reset();native_events=0;native_ready=false;sampled=false;capture_next=false;
    if(!keyboard_raw_claim(&quake_owner)){kputs("quake: native keyboard is busy\n");return;}
    kconsole_set_focus(task_current()->id);ime_bypass(true);fbcon_pause(true);screen=true;task_sleep(60);
    display_set_target(NULL);display_clear(COLOR_BLACK);
    display_fill_rect(0,0,160,720,RGB565(15,22,30));display_fill_rect(1120,0,160,720,RGB565(15,22,30));
    display_draw_text(48,284,"MOVE",COLOR_WHITE,RGB565(15,22,30),2);
    display_draw_text(1168,188,"LOOK",COLOR_WHITE,RGB565(15,22,30),2);
    paint_mask=0;paint_buttons(0,true);
    frames=0;began=hal_time_now_us();
    if(!setjmp(escape)) {
        smooth_active=smooth_requested&&display_smooth_begin(quake_owner);
        if(ppa_test)quake_exit(0);
        previous_frame=controls_only?NULL:quake_malloc(64000);palette_dirty=true;
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
        char *args[9]={"quake","-basedir",quake_basedir,"-nocdaudio"};int ac=4;
        if(silent_requested)args[ac++]="-nosound";
        if(benchmark_start){args[ac++]="+map";args[ac++]=selected_map;}
        args[ac]=NULL;
        COM_InitArgv(ac,args);parms.argc=com_argc;parms.argv=com_argv;
        uint64_t load_at=hal_time_now_us();bool playable_reported=false;
        Host_Init(&parms);
        Cbuf_AddText("bind w +forward\nbind s +back\nbind a +moveleft\nbind d +moveright\nbind f +attack\nbind e \"impulse 10\"\nbind / \"impulse 10\"\nbind UPARROW +forward\nbind DOWNARROW +back\nbind LEFTARROW +left\nbind RIGHTARROW +right\n+mlook\n");
        frames=0;profile_frames=0;began=profile_at=hal_time_now_us();
        kputs("quake: Host_Init complete; interactive loop\n");
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
    const char *base=quake_storage_base();bool map_set=false;strcpy(selected_map,"start");
    for(int i=1;i<argc;i++){
        const char *arg=argv[i];
        if(!strcmp(arg,"--silent")){silent_requested=true;continue;}
        if(!strcmp(arg,"smooth")){smooth_requested=true;benchmark_start=true;continue;}
        if(!strcmp(arg,"bench")){benchmark_start=true;continue;}
        if(!strcmp(arg,"controls")){controls_only=true;continue;}
        if(!strcmp(arg,"ppatest")){controls_only=ppa_test=smooth_requested=true;continue;}
        if(!strcmp(arg,"/sd")||!strcmp(arg,"/assets/quake")){base=arg;continue;}
        size_t n=strlen(arg);bool valid=n>0&&n<sizeof selected_map&&!map_set;
        for(size_t j=0;j<n;j++)if(!((arg[j]>='a'&&arg[j]<='z')||(arg[j]>='0'&&arg[j]<='9')||arg[j]=='_'))valid=false;
        if(!valid){kputs("usage: quake [smooth|bench|controls|ppatest] [MAP] [--silent]\n");return 1;}
        strcpy(selected_map,arg);map_set=true;benchmark_start=true;
    }
    ksnprintf(quake_basedir,sizeof quake_basedir,"%s",base);
    char path[128];ksnprintf(path,sizeof path,"%s/id1/pak0.pak",base);
    vfs_info_t info;if(!controls_only&&vfs_stat(path,&info)<0){kprintf("quake: missing %s; use quake receive BYTES CRC32hex\n",path);return 1;}
    pending=true;uint32_t id;int rc=app_start("quake",&id);if(rc<0)pending=false;return rc<0;
}
#else
void quake_main(void *arg) {(void)arg;}
int quake_command(int argc,char **argv){(void)argc;(void)argv;return -4;}
int quake_app_cleanup(uint32_t id){(void)id;return 0;}
#endif
