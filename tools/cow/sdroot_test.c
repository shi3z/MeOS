#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m5os/kmalloc.h>
#include <m5os/mutex.h>
#include <m5os/sdcard.h>
#include <m5os/task.h>
#include <m5os/vfs.h>
#define SECTORS 65536u
static unsigned char *card;
static unsigned probe_failures,probe_attempts,probe_delays;
static uint64_t fake_time;
uint64_t hal_time_now_us(void){return fake_time+=1000;}
void hal_delay_us(uint32_t us){assert(us==100000);probe_delays++;}
static blockdev_t device;
static int read_sd(blockdev_t *d,uint32_t l,uint32_t n,void *p){(void)d;assert(l<SECTORS&&n<=SECTORS-l);memcpy(p,card+(size_t)l*512,n*512);return 0;}
static int write_sd(blockdev_t *d,uint32_t l,uint32_t n,const void *p){(void)d;assert(l<SECTORS&&n<=SECTORS-l);memcpy(card+(size_t)l*512,p,n*512);return 0;}
int vfs_mount(const char *p,const vfs_fs_t *f,void *d);
static bool fat_busy;
bool fat_is_mounted(void){return fat_busy;}
blockdev_t *sdcard_probe(void){probe_attempts++;if(probe_failures){probe_failures--;return NULL;}return &device;}
void sdcard_status(sdcard_status_t *s){*s=(sdcard_status_t){.probed=true,.ready=true,.sectors=SECTORS,.high_capacity=true,.detail="test probe"};}
void *kmalloc(size_t n){return malloc(n);}
void *kmalloc_psram(size_t n){return malloc(n);}
void *kcalloc(size_t n,size_t z){return calloc(n,z);}
void kfree(void *p){free(p);}
static task_t host_task;
task_t *task_current(void){return &host_task;}
void mutex_lock(mutex_t *m){assert(!m->owner);m->owner=(void *)1;}
void mutex_unlock(mutex_t *m){assert(m->owner);m->owner=NULL;}
void kprintf(const char *fmt,...){va_list ap;va_start(ap,fmt);vprintf(fmt,ap);va_end(ap);}
void kputs(const char *s){fputs(s,stdout);}
/* Include adapter to model process restart by closing its static file handles. */
#include "../../fs/sdroot/sdroot.c"
int vfs_mount(const char *p,const vfs_fs_t *f,void *d){(void)d;assert(!strcmp(p,"/assets")&&f==&assets_vfs);return 0;}
static void verify(const char *text){
    int fd=fs_open("/home/sd-boot.txt",FS_O_READ);assert(fd>=0);
    char b[80]={0};int n=fs_read(fd,b,sizeof b);
    assert(n==(int)strlen(text)&&!memcmp(b,text,(size_t)n));assert(!fs_close(fd));
}
int main(void){
    card=malloc((size_t)SECTORS*512);assert(card);memset(card,0xff,(size_t)SECTORS*512);
    device=(blockdev_t){.name="test-sd",.block_size=512,.block_count=SECTORS,.read=read_sd,.write=write_sd};
    probe_failures=3;assert(sdroot_mount()<0);assert(probe_attempts==3&&probe_delays==2);
    assert(sdroot_mount()<0);assert(sdroot_format(SECTORS-1)<0);
    fat_busy=true;assert(sdroot_format(SECTORS)<0);fat_busy=false;
    assert(!sdroot_format(SECTORS));
    probe_failures=2;unsigned attempts=probe_attempts,delays=probe_delays;
    assert(!sdroot_mount());assert(probe_attempts-attempts==3&&probe_delays-delays==2);
    assert(!sdroot_save_probe("first-boot"));verify("first-boot");
    assert(!sdroot_assets_mount());
    assert(!asset_mkdir(NULL,"/quake"));assert(!asset_mkdir(NULL,"/quake/id1"));
    void *asset=NULL;assert(asset_open(NULL,"/../base.img",VFS_O_READ,&asset)==ERR_INVAL);
    assert(asset_open(NULL,"/base.img",VFS_O_READ,&asset)==ERR_NOENT);
    assert(!asset_open(NULL,"/quake/id1/pak0.pak",VFS_O_WRITE|VFS_O_CREATE,&asset));
    unsigned char chunk[4096];
    for(unsigned i=0;i<sizeof chunk;i++)chunk[i]=(unsigned char)(i*7);
    const unsigned large=18u*1024*1024+123;
    for(unsigned pos=0;pos<large;){unsigned n=large-pos;if(n>sizeof chunk)n=sizeof chunk;
        assert(asset_write(asset,chunk,n)==(int)n);pos+=n;
        if(!(pos%(1024*1024)))assert(!sdroot_save_probe("first-boot"));
    }
    assert(!asset_close(asset));verify("first-boot");
    vfs_info_t info;assert(!asset_stat(NULL,"/quake/id1/pak0.pak",&info)&&info.size==large);

    assert(!fs_unmount());close_files();
    assert(!sdroot_mount());verify("first-boot");
    assert(!asset_open(NULL,"/quake/id1/pak0.pak",VFS_O_READ,&asset));
    assert(asset_seek(asset,(int32_t)large-123,0)==(int)large-123);
    unsigned char check[4096];assert(asset_read(asset,check,sizeof check)==123&&!memcmp(chunk,check,123));
    assert(asset_read(asset,check,1)==0);assert(asset_seek(asset,4096*7+37,0)==4096*7+37);
    assert(asset_read(asset,check,100)==100&&!memcmp(chunk+37,check,100));
    assert(!asset_close(asset));

    for(int i=0;i<30;i++)assert(!sdroot_save_probe("second-boot"));
    assert(!cow_store_compact(&store));verify("second-boot");
    assert(!fs_unmount());close_files();assert(!sdroot_mount());verify("second-boot");
    assert(!fs_unmount());close_files();free(card);
    puts("SDROOT littlefs integration PASS: explicit format, wrong-card size rejection, sparse base, indexed COW, remount persistence, continued saves, compaction, 18 MiB bulk file and random seeks");
}
