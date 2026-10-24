#include <m5os/board_config.h>
#include <m5os/player.h>
#include <string.h>
bool mp3_is_name(const char *name){
    if(!name)return false;
    size_t n=strlen(name);
    return n>=4 && name[n-4]=='.' && (name[n-3]=='m'||name[n-3]=='M') &&
        (name[n-2]=='p'||name[n-2]=='P') && name[n-1]=='3';
}
#if M5OS_BOARD_TAB5
#include "mp3_stream.h"
#include <m5os/audio.h>
#include <m5os/app.h>
#include <m5os/arch.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/vfs.h>
#include <m5os/display.h>
#include <m5os/kprintf.h>
#include "../../kernel/app/app_internal.h"
#include "../../drivers/lcd/fbcon.h"
#include "../tui/dqwin.h"
#define RING 8192u
struct player {
    uint32_t app_id;
    volatile uint32_t read,write,played,underruns;
    volatile unsigned volume;
    volatile bool paused;
    bool display,previous_pause;
    int fd;
    char path[VFS_PATH_MAX];
    mp3_stream_t *stream;
    int16_t ring[RING*2];
};
static struct player *volatile s_player;
static int s_result;
static const char s_display;
static bool s_starting;
static int read_source(void *ctx,void *buf,size_t len){return vfs_read(((struct player *)ctx)->fd,buf,len);}
static int seek_source(void *ctx,uint32_t offset){return vfs_seek(((struct player *)ctx)->fd,(int32_t)offset,FS_SEEK_SET);}
static void render(int16_t *out,unsigned frames,void *arg){
    struct player *p=arg;
    uint32_t irq=arch_irq_disable();
    unsigned first=p->read,available=p->write-first,volume=p->volume;
    bool paused=p->paused;
    unsigned take=paused?0:(available<frames?available:frames);
    arch_irq_restore(irq);
    for(unsigned i=0;i<take;i++){
        unsigned pos=(first+i)%RING;
        int32_t mono=((int32_t)p->ring[pos*2]+p->ring[pos*2+1])/2;
        int16_t value=(int16_t)(mono*(int32_t)volume/100);
        out[i*2]=out[i*2+1]=value;
    }
    irq=arch_irq_disable();
    p->read=first+take;p->played+=take;
    if(!paused && take<frames)p->underruns++;
    arch_irq_restore(irq);
    memset(out+take*2,0,(frames-take)*2*sizeof *out);
}
static void enqueue(struct player *p,const int16_t *pcm,unsigned frames){
    /* Single producer/consumer: data is published only after its copy completes. */
    unsigned first=p->write;
    for(unsigned i=0;i<frames;i++){
        unsigned pos=(first+i)%RING;p->ring[pos*2]=pcm[i*2];p->ring[pos*2+1]=pcm[i*2+1];
    }
    uint32_t irq=arch_irq_disable();p->write=first+frames;arch_irq_restore(irq);
}
static void draw(struct player *p){
    char info[120];
    dq_window(0,0,dq_cols(),dq_rows());dq_text(2,1,"MeOS MP3 PLAYER (read-only)");
    /* Draw clipped path via display's own clipping; never shorten the stored path. */
    dq_text(2,4,p->path);
    ksnprintf(info,sizeof info,"%u Hz / %u channel(s) -> 24000 Hz mono",mp3_stream_rate(p->stream),mp3_stream_channels(p->stream));dq_text(2,7,info);
    ksnprintf(info,sizeof info,"%s  %u:%02u  volume %u%%  underruns %u",p->paused?"Paused":"Playing",
        p->played/AUDIO_RATE/60,(p->played/AUDIO_RATE)%60,p->volume,p->underruns);dq_text(2,9,info);
    dq_text(2,12,"Space:pause  +/-:volume  I:status  Q/Esc:stop");
}
static void player_task(void *arg){
    struct player *p=arg;int error=ERR_OK;bool output=false,eof=false;
    p->fd=vfs_open(p->path,VFS_O_READ);
    if(p->fd<0){error=p->fd;goto done;}
    if(!display_try_claim(&s_display)){error=ERR_BUSY;goto done;}
    p->display=true;p->previous_pause=fbcon_is_paused();fbcon_pause(true);task_sleep(100);
    display_set_target(NULL);kconsole_set_focus(task_current()->id);
    mp3_stream_init(p->stream,read_source,seek_source,p);
    int16_t pcm[512];unsigned since_draw=0;
    for(;;){
        int key=dq_pollkey();
        if(key=='q'||key=='Q'||key==DQK_ESC)break;
        if(key==' '){
            p->paused=!p->paused;
            kprintf("mp3: %s at %u frames\n",p->paused?"paused":"resumed",p->played);
        }
        if((key=='+'||key=='=')&&p->volume<100)p->volume+=5;
        if(key=='-'&&p->volume>=5)p->volume-=5;
        if(key=='+'||key=='='||key=='-')kprintf("mp3: volume %u%%\n",p->volume);
        if(key=='i'||key=='I')kprintf("mp3: %s, played %u frames, volume %u%%, underruns %u\n",
            p->paused?"paused":"playing",p->played,p->volume,p->underruns);
        unsigned queued=p->write-p->read;
        if(!eof && queued<=RING-256){
            int n=mp3_stream_read(p->stream,pcm,256);
            if(n<0){error=n;break;}
            if(!n)eof=true;else enqueue(p,pcm,(unsigned)n);
        }
        queued=p->write-p->read;
        if(!output && (queued>=4096 || (eof&&queued))){
            if(!audio_open(p->app_id,render,p)){error=ERR_BUSY;break;}
            output=true;draw(p);
            kprintf("mp3: %u Hz, %u channel(s), 24kHz output; Q stops\n",mp3_stream_rate(p->stream),mp3_stream_channels(p->stream));
        }
        if(output && eof && !queued)break;
        if(!output && eof){error=ERR_CORRUPT;break;}
        if(output && ++since_draw>=32){draw(p);since_draw=0;}
        if(queued>RING-256 || p->paused)task_sleep(5);
    }
done:
    if(output)audio_close(p->app_id);
    task_stack_info_t stack;
    if(task_stack_inspect(task_current()->id,&stack)==ERR_OK)
        kprintf("mp3: stack estimate %u/%u bytes, guard %s\n",(unsigned)stack.used,(unsigned)stack.size,stack.guard_ok?"OK":"FAILED");
    s_result=error;
    kprintf("mp3: stopped (%d), played %u frames, %u underruns\n",error,p->played,p->underruns);
    /* appd owns the final handle/display/memory cleanup, including forced stops. */
}
int mp3_app_cleanup(uint32_t id){
    struct player *p=s_player;if(!p || p->app_id!=id)return ERR_OK;
    audio_close(id);
    if(p->fd>=0){vfs_close(p->fd);p->fd=-1;}
    if(p->display){
        display_set_target(NULL);kconsole_set_focus(0);fbcon_pause(p->previous_pause);display_release_claim(&s_display);
    }
    uint32_t irq=arch_irq_disable();s_player=NULL;arch_irq_restore(irq);return ERR_OK;
}
int mp3_play_file(const char *path){
    task_t *caller=task_current();if(!caller || caller->app)return ERR_PERM;
    char normalized[VFS_PATH_MAX];if(vfs_normalize(path,normalized)!=ERR_OK)return ERR_INVAL;
    uint32_t irq=arch_irq_disable();
    if(s_player || s_starting){arch_irq_restore(irq);return ERR_BUSY;}s_starting=true;arch_irq_restore(irq);
    app_t *app=app_reserve("mp3",APP_PERM_CONSOLE|APP_PERM_DISPLAY|APP_PERM_INPUT|APP_PERM_FS_READ|APP_PERM_SPAWN);
    if(!app){s_starting=false;return ERR_BUSY;}
    struct player *p=kmalloc_psram_owned(sizeof *p,app->app_id);
    mp3_stream_t *stream=kmalloc_psram_owned(mp3_stream_bytes(),app->app_id);
    if(!p || !stream){app_abort(app);s_starting=false;return ERR_NOMEM;}
    memset(p,0,sizeof *p);p->fd=-1;p->app_id=app->app_id;p->stream=stream;p->volume=50;
    memcpy(p->path,normalized,strlen(normalized)+1);
    irq=arch_irq_disable();s_player=p;s_result=ERR_IO;s_starting=false;arch_irq_restore(irq);
    int rc=app_launch(app,player_task,p,TASK_PRIO_DEFAULT+1,32768);
    if(rc<0)return rc; /* app_abort uses the registered player cleanup hook */
    while(s_player==p)task_sleep(10);
    return s_result;
}
#else
#include <m5os/errno.h>
int mp3_play_file(const char *path){(void)path;return ERR_NOSYS;}
int mp3_app_cleanup(uint32_t id){(void)id;return ERR_OK;}
#endif
