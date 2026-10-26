#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <m5os/vfs.h>
#define QUAKE_COMPAT_DECLARATIONS_ONLY
#include "../../components/quakeengine/compat.h"
#include "../../third_party/quakegeneric/source/quakedef.h"
#include "../../third_party/quakegeneric/source/quakegeneric.h"
uint32_t quake_owner=17;char quake_basedir[96]="/sd";
static jmp_buf done;
static unsigned frames;
static uint64_t render_hash=UINT64_C(14695981039346656037);
static uint64_t depth_hash=UINT64_C(14695981039346656037);
extern short *d_pzbuffer;
extern unsigned int d_zwidth;
static unsigned depth_frames,padding_checks;
static int padding_ready;
static unsigned hashed_frames;
static const char *data_root,*image_path;
static unsigned char pal[768];
void *kmalloc_psram_owned(size_t n,uint32_t owner){(void)owner;return malloc(n);}
void kfree(void *p){free(p);}
void kputs(const char *s){fputs(s,stdout);}
void kprintf(const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);va_end(a);}
void quake_exit(int code){longjmp(done,code?2:1);}
uint64_t hal_time_now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
void task_sleep(uint32_t ms){(void)ms;}
int vfs_normalize(const char *path,char *out){if(strlen(path)>=VFS_PATH_MAX||strstr(path,".."))return -1;strcpy(out,path);return 0;}
static const char *native(const char *p){static char result[2048];snprintf(result,sizeof result,"%s%s",data_root,p+3);return result;}
int vfs_stat(const char *p,vfs_info_t *out){struct stat s;if(stat(native(p),&s))return -1;*out=(vfs_info_t){.type=S_ISREG(s.st_mode)?FS_TYPE_FILE:FS_TYPE_DIR,.size=(uint32_t)s.st_size};return 0;}
int vfs_open(const char *p,uint32_t flags){(void)flags;return open(native(p),O_RDONLY);}
int vfs_close(int fd){return close(fd);}
int vfs_read(int fd,void *p,size_t n){return (int)read(fd,p,n);}
int vfs_seek(int fd,int32_t off,int whence){return (int)lseek(fd,off,whence);}
void QG_Init(void){}
void QG_Quit(void){quake_exit(0);}
void QG_SetPalette(unsigned char p[768]){memcpy(pal,p,768);}
void QG_DrawFrame(void *pixels){
 frames++;
 if(d_pzbuffer&&d_zwidth){
  if(d_zwidth!=QV_Z_PITCH)quake_exit(1);
  for(unsigned y=0;y<QV_HEIGHT;y++)for(unsigned x=QV_WIDTH;x<QV_Z_PITCH;x++){
   short *z=d_pzbuffer+y*QV_Z_PITCH+x;
   if(*z!=(padding_ready?(short)0x5a7f:0))quake_exit(1);
   *z=(short)0x5a7f;
  }
  padding_ready=1;padding_checks++;
 }
 /* Startup consoles contain the selected cache-size text. Compare gameplay
  * only, after signon and console animation have finished. */
 if(cls.signon==SIGNONS&&!scr_con_current){
  hashed_frames++;
  for(unsigned i=0;i<QV_PIXELS;i++)render_hash=(render_hash^((unsigned char *)pixels)[i])*UINT64_C(1099511628211);
  for(unsigned i=0;i<768;i++)render_hash=(render_hash^pal[i])*UINT64_C(1099511628211);
  if(!d_pzbuffer||d_zwidth!=QV_Z_PITCH)quake_exit(1);
  depth_frames++;
  for(unsigned y=0;y<QV_HEIGHT;y++)for(unsigned x=0;x<QV_WIDTH;x++){
   uint16_t z=(uint16_t)d_pzbuffer[y*QV_Z_PITCH+x];
   depth_hash=(depth_hash^(z&255))*UINT64_C(1099511628211);
   depth_hash=(depth_hash^(z>>8))*UINT64_C(1099511628211);
  }
 }
 if(frames==20){FILE *f=fopen(image_path,"wb");if(!f)quake_exit(1);fprintf(f,"P6\n%d %d\n255\n",QV_WIDTH,QV_HEIGHT);for(unsigned i=0;i<QV_PIXELS;i++)fwrite(pal+((unsigned char *)pixels)[i]*3,1,3,f);fclose(f);}
}
int QG_GetKey(int *down,int *key){(void)down;(void)key;return 0;}
void QG_GetMouseMove(int *x,int *y){*x=*y=0;}
void QG_GetJoyAxes(float *a){memset(a,0,6*sizeof *a);}
int main(int argc,char **argv){
 if(argc!=5)return 2;data_root=argv[1];image_path=argv[2];
 unsigned steps=(unsigned)strtoul(argv[3],NULL,10);
 if(!steps||steps>3000)return 2;
 setvbuf(stdout,NULL,_IONBF,0);
 int result=setjmp(done);if(result)return result==1?0:1;
 quakeparms_t p={0};p.memsize=12*1024*1024;p.membase=quake_malloc(p.memsize);p.basedir=quake_basedir;
 char *args[]={"quake","-basedir",quake_basedir,"-nosound","-nocdaudio","-surfcachesize",argv[4],NULL};COM_InitArgv(7,args);p.argc=com_argc;p.argv=com_argv;
 quake_profile_reset();Host_Init(&p);for(unsigned i=0;i<steps;i++)Host_Frame(.05);
 quake_profile_stats profile=quake_profile_take();
 if(!profile.completed_frames||!profile.phase_us[QP_RENDER]||!profile.read_bytes||
    !profile.cache[QC_HIT]||!profile.cache[QC_REBUILD]||profile.cache[QC_ALLOC]>profile.cache[QC_REBUILD])return 1;
 printf("HEADLESS profile: %u completed frames, %u bytes read\n",profile.completed_frames,(unsigned)profile.read_bytes);
#ifdef MEOS_QUAKE_DETAIL_PROFILE
 if(!profile.detail_calls[QD_EDGES]||!profile.detail_calls[QD_CACHE]||!profile.detail_calls[QD_SPANS])return 1;
 printf("HEADLESS detail scopes: edges=%u cache=%u spans=%u\n",profile.detail_calls[QD_EDGES],profile.detail_calls[QD_CACHE],profile.detail_calls[QD_SPANS]);
#endif
 printf("HEADLESS cache: hit=%llu rebuild=%llu alloc=%llu evict=%llu wrap=%llu thrash=%llu pixels=%llu bytes=%llu dynamic=%llu texture=%llu lightstyle=%llu\n",
 (unsigned long long)profile.cache[QC_HIT],(unsigned long long)profile.cache[QC_REBUILD],
 (unsigned long long)profile.cache[QC_ALLOC],(unsigned long long)profile.cache[QC_EVICT],
 (unsigned long long)profile.cache[QC_WRAP],(unsigned long long)profile.cache[QC_THRASH],
 (unsigned long long)profile.cache[QC_PIXELS],(unsigned long long)profile.cache[QC_BYTES],
 (unsigned long long)profile.cache[QC_DYNAMIC],(unsigned long long)profile.cache[QC_TEXTURE],(unsigned long long)profile.cache[QC_LIGHTSTYLE]);
 printf("HEADLESS render hash: %016llx (%u gameplay frames)\n",(unsigned long long)render_hash,hashed_frames);
 printf("HEADLESS depth hash: %016llx (%u gameplay frames), pitch=%u padding checks=%u\n",
 (unsigned long long)depth_hash,depth_frames,QV_Z_PITCH,padding_checks);
 printf("HEADLESS frames=%u\n",frames);return frames>=20?0:1;
}
