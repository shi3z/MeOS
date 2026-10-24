#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m5os/app.h>
#include <m5os/audio.h>
#include <m5os/player.h>
#include <m5os/display.h>
#include <m5os/vfs.h>
#include <m5os/errno.h>
#include "../../user/player/mp3_stream.h"
static app_t app;static task_t caller,worker;static task_t *current=&caller;
static void *allocs[2];static unsigned alloc_count,fail_alloc,depth,blocks,closes,polls,scenario,produced;
static bool display,paused,active;static audio_render_t callback;static void *callback_arg;
static jmp_buf killed;
uint32_t arch_irq_disable(void){return depth++;}
void arch_irq_restore(uint32_t old){assert(depth==old+1);depth=old;}
task_t *task_current(void){return current;}
void kprintf(const char *fmt,...){(void)fmt;}
void kconsole_set_focus(uint32_t id){(void)id;}
int ksnprintf(char *out,size_t n,const char *fmt,...){va_list ap;va_start(ap,fmt);int r=vsnprintf(out,n,fmt,ap);va_end(ap);return r;}
void *kmalloc_psram_owned(size_t n,uint32_t id){assert(id==app.app_id);if(++alloc_count==fail_alloc)return NULL;void *p=malloc(n);assert(alloc_count<=2);allocs[alloc_count-1]=p;return p;}
static void reclaim(void){assert(!active);for(unsigned i=0;i<2;i++){free(allocs[i]);allocs[i]=NULL;}}
app_t *app_reserve(const char *name,uint32_t perms){assert(!strcmp(name,"mp3") && (perms&APP_PERM_FS_READ) && !(perms&APP_PERM_FS_WRITE));app=(app_t){.app_id=42,.state=APP_RUNNING};return &app;}
void app_abort(app_t *a){assert(a==&app);assert(mp3_app_cleanup(a->app_id)==ERR_OK);reclaim();}
int app_launch(app_t *a,void (*entry)(void *),void *arg,uint8_t priority,size_t stack){
    assert(a==&app && priority==2 && stack==32768);
    if(scenario==5){app_abort(a);return ERR_NOMEM;}
    worker.id=7;worker.app=a;current=&worker;
    if(!setjmp(killed))entry(arg);
    current=&caller;assert(mp3_app_cleanup(a->app_id)==ERR_OK);reclaim();return ERR_OK;
}
int task_stack_inspect(uint32_t pid,task_stack_info_t *out){assert(pid==worker.id);*out=(task_stack_info_t){.size=32768,.used=16384,.free=16384,.guard_ok=true};return ERR_OK;}
void task_sleep(uint32_t ms){assert(ms<=100);}
int vfs_normalize(const char *path,char *out){if(strlen(path)>=VFS_PATH_MAX)return ERR_NAMETOOLONG;strcpy(out,path);return ERR_OK;}
int vfs_open(const char *path,uint32_t flags){assert(strstr(path,".mp3") && flags==VFS_O_READ);return 5;}
int vfs_close(int fd){assert(fd==5);assert(!active);closes++;return ERR_OK;}
int vfs_read(int fd,void *out,size_t n){(void)fd;(void)out;(void)n;assert(0);return 0;}
int vfs_seek(int fd,int32_t offset,int whence){(void)fd;(void)whence;return offset;}
bool display_try_claim(const void *owner){assert(owner);if(scenario==4)return false;assert(!display);display=true;return true;}
void display_release_claim(const void *owner){assert(owner&&display);display=false;}
void display_set_target(uint16_t *buf){assert(!buf);}
bool fbcon_is_paused(void){return paused;}
void fbcon_pause(bool value){paused=value;}
int dq_cols(void){return 80;}int dq_rows(void){return 45;}
void dq_window(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
void dq_text(int x,int y,const char *s){(void)x;(void)y;assert(s);}
int dq_pollkey(void){
    polls++;
    if(scenario==6 && polls==22)longjmp(killed,1);
    if(active){int16_t pcm[1024];callback(pcm,512,callback_arg);blocks++;}
    if(scenario==1){if(polls==20)return ' ';if(polls==23)return ' ';if(polls==25)return '+';if(polls==27)return 'q';}
    return -1;
}
bool audio_open(uint32_t owner,audio_render_t fn,void *arg){assert(owner==42);if(scenario==3)return false;assert(!active);active=true;callback=fn;callback_arg=arg;return true;}
void audio_close(uint32_t owner){assert(owner==42);active=false;callback=NULL;callback_arg=NULL;}
size_t mp3_stream_bytes(void){return 16;}
void mp3_stream_init(mp3_stream_t *s,mp3_read_fn rd,mp3_seek_fn sk,void *ctx){assert(s&&rd&&sk&&ctx);produced=0;}
int mp3_stream_read(mp3_stream_t *s,int16_t *pcm,unsigned frames){assert(s && frames==256);if(scenario==2)return ERR_CORRUPT;if(produced>=8192)return 0;for(unsigned i=0;i<frames*2;i++)pcm[i]=1000;produced+=frames;return frames;}
unsigned mp3_stream_rate(const mp3_stream_t *s){(void)s;return 44100;}
unsigned mp3_stream_channels(const mp3_stream_t *s){(void)s;return 2;}
int main(void){
    assert(mp3_is_name("録音.MP3") && !mp3_is_name("song.mp3.txt"));
    for(scenario=0;scenario<=6;scenario++){
        alloc_count=fail_alloc=polls=closes=blocks=0;paused=true;
        int rc=mp3_play_file("/sd/長い名前 with spaces.mp3");
        assert(rc==(scenario==2?ERR_CORRUPT:scenario==3||scenario==4?ERR_BUSY:scenario==5?ERR_NOMEM:scenario==6?ERR_IO:ERR_OK));
        assert(!active&&!display&&paused&&!depth&&closes==(scenario==5?0:1));
    }
    for(unsigned fail=1;fail<=2;fail++){
        alloc_count=0;fail_alloc=fail;assert(mp3_play_file("/sd/tone.mp3")==ERR_NOMEM);assert(!active&&!display);
    }
    puts("player: normal/pause/quit, codec/audio/display failures, forced termination, allocation/launch failures and stop-before-free PASS");
}
