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
static unsigned probe_failures,probe_attempts,probe_delays,read_requests,write_requests,max_read_blocks,max_write_blocks;
static unsigned asset_alloc_attempts;
static bool fail_reads,fail_asset_alloc=true;
static unsigned fail_partial_bytes;
static uint64_t fake_time;
uint64_t hal_time_now_us(void){return fake_time+=1000;}
void hal_delay_us(uint32_t us){assert(us==100000);probe_delays++;}
static blockdev_t device;
static int read_sd(blockdev_t *d,uint32_t l,uint32_t n,void *p){
    (void)d;assert(l<SECTORS&&n<=SECTORS-l);if(n>max_read_blocks)max_read_blocks=n;
    read_requests++;
    if(fail_reads)return -1;
    if(fail_partial_bytes){
        size_t take=fail_partial_bytes;if(take>(size_t)n*512)take=(size_t)n*512;
        memset(p,0x5a,take);fail_partial_bytes=0;return -1;
    }
    memcpy(p,card+(size_t)l*512,n*512);return 0;
}
static int write_sd(blockdev_t *d,uint32_t l,uint32_t n,const void *p){(void)d;assert(l<SECTORS&&n<=SECTORS-l);if(n>max_write_blocks)max_write_blocks=n;write_requests++;memcpy(card+(size_t)l*512,p,n*512);return 0;}
static bool fat_busy;
bool fat_is_mounted(void){return fat_busy;}
blockdev_t *sdcard_probe(void){probe_attempts++;if(probe_failures){probe_failures--;return NULL;}return &device;}
void sdcard_status(sdcard_status_t *s){*s=(sdcard_status_t){.probed=true,.ready=true,.sectors=SECTORS,.high_capacity=true,.detail="test probe"};}
void *kmalloc(size_t n){return malloc(n);}
void *kmalloc_psram(size_t n){
    if(n>1024u*1024u){asset_alloc_attempts++;if(fail_asset_alloc)return NULL;}
    return malloc(n);
}
void *kcalloc(size_t n,size_t z){return calloc(n,z);}
void kfree(void *p){free(p);}
static task_t host_task;
task_t *task_current(void){return &host_task;}
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
static void verify_asset_read(bool cached){
    enum { BYTES=128*1024 };unsigned char *src=malloc(BYTES),*dst=malloc(BYTES);assert(src&&dst);
    for(size_t i=0;i<BYTES;i++)src[i]=(unsigned char)(i*29u+(i>>9));
    void *file=NULL;assert(!asset_open(NULL,"/readahead.bin",VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC,&file));
    assert(asset_write(file,src,BYTES)==BYTES);assert(!asset_close(file));
    sdroot_io_reset();file=NULL;assert(!asset_open(NULL,"/readahead.bin",VFS_O_READ,&file));
    assert(asset_read(file,dst,BYTES)==BYTES);assert(!memcmp(src,dst,BYTES));assert(!asset_close(file));
    if(cached) {
        assert(io_stats.cache_misses>0&&io_stats.cache_hits>io_stats.cache_misses);
        assert(io_stats.sector_hits>0&&io_stats.sector_misses>0);
        assert(io_stats.physical_calls<io_stats.calls&&max_read_blocks>=READ_AHEAD_BYTES/512u);
    } else {
        assert(!io_stats.cache_hits&&!io_stats.cache_misses);
        assert(!io_stats.sector_hits&&!io_stats.sector_misses);
        assert(io_stats.physical_calls==io_stats.calls);
    }
    assert(!asset_unlink(NULL,"/readahead.bin"));free(dst);free(src);
}
static void verify_asset_lifecycle(void){
    enum { BYTES=128*1024, CHUNK=16*1024, CYCLES=64 };
    unsigned char *src=malloc(BYTES),*dst=malloc(CHUNK);assert(src&&dst);
    for(size_t i=0;i<BYTES;i++)src[i]=(unsigned char)(i*73u+(i>>9)*11u);
    void *file=NULL;
    assert(!asset_open(NULL,"/lifecycle.bin",VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC,&file));
    assert(asset_write(file,src,BYTES)==BYTES);assert(!asset_close(file));
    assert(!asset_open(NULL,"/lifecycle.bin",VFS_O_READ,&file));
    for(unsigned cycle=0;cycle<CYCLES;cycle++){
        size_t cache_bytes=sdroot_asset_cache_bytes();assert(cache_bytes==sizeof *asset_buffers);
        assert(sdroot_asset_cache_trim()==cache_bytes&&!sdroot_asset_cache_bytes());
        /* The same open handle must survive release, failed recreation and
         * direct reads; an empty trim explicitly rearms the next attempt. */
        fail_asset_alloc=(cycle%2)==0;
        unsigned attempts=asset_alloc_attempts;
        uint32_t offset=((cycle*5u)%(BYTES/CHUNK))*CHUNK;
        assert(asset_seek(file,(int32_t)offset,FS_SEEK_SET)==(int)offset);
        assert(asset_read(file,dst,CHUNK)==CHUNK&&!memcmp(dst,src+offset,CHUNK));
        assert(asset_alloc_attempts==attempts+1);
        if(fail_asset_alloc){
            assert(!sdroot_asset_cache_bytes());fail_asset_alloc=false;
            assert(asset_seek(file,0,FS_SEEK_SET)==0);
            assert(asset_read(file,dst,CHUNK)==CHUNK&&!memcmp(dst,src,CHUNK));
            assert(asset_alloc_attempts==attempts+1&&!sdroot_asset_cache_bytes());
            assert(!sdroot_asset_cache_trim());
            assert(asset_read(file,dst,CHUNK)==CHUNK&&!memcmp(dst,src+CHUNK,CHUNK));
            assert(asset_alloc_attempts==attempts+2);
        }
        assert(sdroot_asset_cache_bytes()==cache_bytes);
    }
    assert(!asset_close(file));
    /* Reuse the cached file's blocks for changed content. Writes must invalidate
     * the cache before a later reopen/seek can return the new bytes. */
    for(unsigned pass=0;pass<8;pass++){
        uint32_t offset=pass*CHUNK;
        memset(src+offset,0x80u+pass,CHUNK);
        assert(!asset_open(NULL,"/lifecycle.bin",VFS_O_READ|VFS_O_WRITE,&file));
        assert(asset_seek(file,(int32_t)offset,FS_SEEK_SET)==(int)offset);
        assert(asset_write(file,src+offset,CHUNK)==CHUNK);assert(!asset_close(file));
        assert(!asset_open(NULL,"/lifecycle.bin",VFS_O_READ,&file));
        for(unsigned block=0;block<BYTES/CHUNK;block++){
            assert(asset_read(file,dst,CHUNK)==CHUNK&&!memcmp(dst,src+block*CHUNK,CHUNK));
        }
        assert(!asset_close(file));
    }
    assert(!asset_unlink(NULL,"/lifecycle.bin"));free(dst);free(src);
    printf("SDROOT asset lifecycle PASS: %u trim/recreate cycles, %u allocation failures, open-handle continuity, bounded retries, write invalidation\n",CYCLES,CYCLES/2);
}
static void verify_partial_refill_errors(void){
    unsigned char dst[ERASE_BYTES];
    sdroot_io_reset();mutex_lock(&asset_read_mutex);asset_read_owner=task_current();
    /* Fill both windows, then fail a third-region replacement after DMA has
     * already changed part of the evicted slot. Its old tag must not survive. */
    assert(!disk_read(&cfg,1024,0,dst,sizeof dst));
    assert(!disk_read(&cfg,1040,0,dst,sizeof dst));
    fail_partial_bytes=512;
    assert(disk_read(&cfg,1056,0,dst,sizeof dst)==LFS_ERR_IO);
    assert(!disk_read(&cfg,1024,0,dst,sizeof dst));
    assert(!memcmp(dst,card+(size_t)1024*ERASE_BYTES,sizeof dst));
    asset_read_owner=NULL;mutex_unlock(&asset_read_mutex);

    sdroot_io_reset();mutex_lock(&asset_read_mutex);asset_read_owner=task_current();
    /* Find enough collisions to replace a four-way set, independently of
     * the index function's treatment of aligned sectors. */
    uint32_t colliding[SECTOR_CACHE_WAYS+1];unsigned count=0;
    unsigned set=sector_cache_set(1024*8+1);
    for(uint32_t b=1024;b<SECTORS/8&&count<SECTOR_CACHE_WAYS+1;b++)
        if(sector_cache_set(b*8+1)==set)colliding[count++]=b;
    assert(count==SECTOR_CACHE_WAYS+1);
    for(unsigned way=0;way<SECTOR_CACHE_WAYS;way++)
        assert(!disk_read(&cfg,colliding[way],512,dst,512));
    fail_partial_bytes=256;
    assert(disk_read(&cfg,colliding[SECTOR_CACHE_WAYS],512,dst,512)==LFS_ERR_IO);
    unsigned before=read_requests;
    assert(!disk_read(&cfg,colliding[0],512,dst,512));
    assert(read_requests==before+1);
    assert(!memcmp(dst,card+(size_t)colliding[0]*ERASE_BYTES+512,512));
    asset_read_owner=NULL;mutex_unlock(&asset_read_mutex);
    puts("SDROOT partial-refill error PASS: read-ahead and sector slots discard overwritten tags");
}
static void verify_aligned_sector_working_set(void){
    enum { HEADERS=1024 };
    unsigned char dst[512];
    sdroot_io_reset();mutex_lock(&asset_read_mutex);asset_read_owner=task_current();
    unsigned before=read_requests;
    /* A 512 KiB working set of 4 KiB-aligned headers fits in the 1 MiB
     * sector cache. Indexing only low sector bits collapses it into 64 sets
     * and rereads every header on the second pass. */
    for(unsigned pass=0;pass<2;pass++){
        for(unsigned i=0;i<HEADERS;i++){
            uint32_t block=1024+i;
            assert(!disk_read(&cfg,block,0,dst,sizeof dst));
            assert(!memcmp(dst,card+(size_t)block*ERASE_BYTES,sizeof dst));
        }
        assert(read_requests-before==HEADERS);
    }
    assert(io_stats.sector_hits==HEADERS&&io_stats.sector_misses==HEADERS);
    asset_read_owner=NULL;mutex_unlock(&asset_read_mutex);
    puts("SDROOT aligned sector working set PASS: 1024 headers, no second-pass physical reads");
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
    assert(!asset_buffers);
    assert(!sdroot_assets_mount());
    verify_asset_read(false);
    unsigned failed_attempts=asset_alloc_attempts;fail_asset_alloc=false;
    verify_asset_read(false);assert(asset_alloc_attempts==failed_attempts);
    assert(!fs_unmount());close_files();fail_asset_alloc=false;
    assert(!sdroot_mount()&&!asset_buffers);assert(!sdroot_assets_mount()&&asset_buffers);
    verify_asset_read(true);
    size_t cache_bytes=sdroot_asset_cache_bytes();assert(cache_bytes==sizeof *asset_buffers);
    assert(sdroot_asset_cache_trim()==cache_bytes&&!sdroot_asset_cache_bytes()&&!asset_buffers);
    verify_asset_read(true);assert(sdroot_asset_cache_bytes()==cache_bytes);
    verify_asset_lifecycle();
    verify_partial_refill_errors();
    verify_aligned_sector_working_set();
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
