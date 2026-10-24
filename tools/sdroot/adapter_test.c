#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m5os/kmalloc.h>
#include <m5os/mutex.h>
#include <m5os/sdcard.h>
#include <m5os/vfs.h>
#define SECTORS 65536u
static unsigned char *card;
static unsigned probe_failures,probe_attempts,probe_delays,write_requests,max_read_blocks,max_write_blocks;
static bool fail_reads;
static uint64_t fake_time;
uint64_t hal_time_now_us(void){return fake_time+=1000;}
void hal_delay_us(uint32_t us){assert(us==100000);probe_delays++;}
static blockdev_t device;
static int read_sd(blockdev_t *d,uint32_t l,uint32_t n,void *p){(void)d;assert(l<SECTORS&&n<=SECTORS-l);if(n>max_read_blocks)max_read_blocks=n;if(fail_reads)return -1;memcpy(p,card+(size_t)l*512,n*512);return 0;}
static int write_sd(blockdev_t *d,uint32_t l,uint32_t n,const void *p){(void)d;assert(l<SECTORS&&n<=SECTORS-l);if(n>max_write_blocks)max_write_blocks=n;write_requests++;memcpy(card+(size_t)l*512,p,n*512);return 0;}
static bool fat_busy;
bool fat_is_mounted(void){return fat_busy;}
blockdev_t *sdcard_probe(void){probe_attempts++;if(probe_failures){probe_failures--;return NULL;}return &device;}
void sdcard_status(sdcard_status_t *s){*s=(sdcard_status_t){.probed=true,.ready=true,.sectors=SECTORS,.high_capacity=true,.detail="test probe"};}
void *kmalloc(size_t n){return malloc(n);}
void *kmalloc_psram(size_t n){return malloc(n);}
void *kcalloc(size_t n,size_t z){return calloc(n,z);}
void kfree(void *p){free(p);}
void mutex_lock(mutex_t *m){assert(!m->owner);m->owner=(void *)1;}
void mutex_unlock(mutex_t *m){assert(m->owner);m->owner=NULL;}
void kprintf(const char *fmt,...){va_list ap;va_start(ap,fmt);vprintf(fmt,ap);va_end(ap);}
void kputs(const char *s){fputs(s,stdout);}
int vfs_mount(const char *path,const vfs_fs_t *fs,void *ctx){
    (void)path;(void)fs;(void)ctx;return 0;
}
/* Include adapter to model process restart by closing its static file handles. */
#include "../../fs/sdroot/sdroot.c"
static void verify(const char *text){
    int fd=fs_open("/home/sd-boot.txt",FS_O_READ);assert(fd>=0);
    char b[80]={0};int n=fs_read(fd,b,sizeof b);
    assert(n==(int)strlen(text)&&!memcmp(b,text,(size_t)n));assert(!fs_close(fd));
}
int main(void){
    card=malloc((size_t)SECTORS*512);assert(card);memset(card,0xff,(size_t)SECTORS*512);
    device=(blockdev_t){.name="test-sd",.block_size=512,.block_count=SECTORS,.read=read_sd,.write=write_sd};
    probe_failures=3;assert(sdroot_mount()<0);assert(probe_attempts==3&&probe_delays==2);
    assert(sdroot_mount()<0);assert(!write_requests);
    fail_reads=true;assert(sdroot_mount()<0);fail_reads=false;assert(!write_requests);
    assert(sdroot_format(SECTORS-1)<0);assert(!write_requests);
    fat_busy=true;unsigned blocked_probes=probe_attempts;
    assert(sdroot_mount()==ERR_BUSY);assert(probe_attempts==blocked_probes&&!write_requests);
    assert(sdroot_format(SECTORS)<0);fat_busy=false;assert(!write_requests);
    assert(!sdroot_format(SECTORS));assert(write_requests>0&&max_write_blocks>=8);
    /* Model the outer littlefs volume being open before M5FS is attached. */
    assert(!lfs_mount(&lfs,&cfg));mounted=true;
    unsigned outer_probes=probe_attempts,outer_writes=write_requests;
    assert(sdroot_mount()==ERR_BUSY);
    assert(probe_attempts==outer_probes&&write_requests==outer_writes);
    close_files();
    probe_failures=2;unsigned attempts=probe_attempts,delays=probe_delays;
    assert(!sdroot_mount());assert(probe_attempts-attempts==3&&probe_delays-delays==2);
    unsigned before=write_requests,probed=probe_attempts;
    assert(sdroot_format(SECTORS)<0);assert(sdroot_mount()<0);
    assert(write_requests==before&&probe_attempts==probed);
    assert(!sdroot_save_probe("first-boot"));verify("first-boot");
    assert(!fs_unmount());close_files();
    assert(!sdroot_mount());verify("first-boot");assert(max_read_blocks>=8);
    for(int i=0;i<30;i++)assert(!sdroot_save_probe("second-boot"));
    assert(!cow_store_compact(&store));verify("second-boot");
    assert(!fs_unmount());close_files();assert(!sdroot_mount());verify("second-boot");
    assert(!fs_unmount());close_files();free(card);
    puts("SDROOT littlefs integration PASS: 4 KiB multi-sector cache/erase, explicit format, wrong-card size rejection, sparse base, indexed COW, remount persistence, continued saves, compaction");
}
