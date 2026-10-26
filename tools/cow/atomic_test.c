#include "../../fs/cow/blockdev.h"
#include "../../fs/cow/store.h"
#include <m5os/fs.h>
#include <m5os/vfs.h>
#include <m5os/app.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/mutex.h>
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t allowed = APP_PERM_FS_READ | APP_PERM_FS_WRITE;
bool task_has_perm(uint32_t p){return (allowed&p)==p;}
task_t *task_current(void){return NULL;}
uint32_t arch_irq_disable(void){return 0;}
void arch_irq_restore(uint32_t flags){(void)flags;}
const vfs_fs_t vfs_devfs={.name="test-dev"};
#define SECTORS 128u
#define CAP (512u+40u*COW_RECORD)
static unsigned char image[SECTORS*512];
struct bank {unsigned char bytes[CAP];};
static struct bank banks[2];
static int budget=-1,fail_sync,syncs,fail_recover;
static jmp_buf replacement_abandoned;
static bool abandon_write;
static size_t allocated;
typedef union {max_align_t align;size_t size;} alloc_header;
void *kmalloc(size_t n){alloc_header *h=malloc(sizeof *h+n);if(!h)return NULL;h->size=n;allocated+=n;return h+1;}
void *kmalloc_psram(size_t n){return kmalloc(n);}
void *kcalloc(size_t n,size_t z){if(z&&n>SIZE_MAX/z)return NULL;void *p=kmalloc(n*z);if(p)memset(p,0,n*z);return p;}
void kfree(void *p){if(p){alloc_header *h=(alloc_header *)p-1;allocated-=h->size;free(h);}}
void kmem_psram_stats(kmem_stats_t *s){*s=(kmem_stats_t){.used=allocated};}
void kprintf(const char *fmt,...){va_list ap;va_start(ap,fmt);vprintf(fmt,ap);va_end(ap);}
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int held;
void mutex_lock(mutex_t *m){(void)m;assert(!held);assert(!pthread_mutex_lock(&lock));held=1;}
void mutex_unlock(mutex_t *m){(void)m;assert(held);held=0;assert(!pthread_mutex_unlock(&lock));}
static int base(void *ctx,uint32_t l,void *p){(void)ctx;assert(l<SECTORS);memcpy(p,image+l*512,512);return 0;}
static int raw_rd(blockdev_t *bd,uint32_t l,uint32_t n,void *p){(void)bd;assert(l+n<=SECTORS);memcpy(p,image+l*512,n*512);return 0;}
static int raw_wr(blockdev_t *bd,uint32_t l,uint32_t n,const void *p){(void)bd;assert(l+n<=SECTORS);memcpy(image+l*512,p,n*512);return 0;}
static int rd(void *ctx,uint32_t o,void *p,size_t n){struct bank *b=ctx;assert(o+n<=CAP);memcpy(p,b->bytes+o,n);return 0;}
static int wr(void *ctx,uint32_t o,const void *p,size_t n){struct bank *b=ctx;assert(o+n<=CAP);size_t take=n;
    if(budget>=0&&(size_t)budget<take)take=(size_t)budget;
    memcpy(b->bytes+o,p,take);if(budget>=0)budget-=(int)take;
    if(abandon_write){abandon_write=false;longjmp(replacement_abandoned,1);}
    return take==n?0:-1;
}
static int reset(void *ctx){struct bank *b=ctx;memset(b,0,sizeof *b);return 0;}
static int sync_io(void *ctx){(void)ctx;return ++syncs==fail_sync?-1:0;}
static int recover(void *ctx){budget=-1;fail_sync=0;return fail_recover?-1:cow_store_abort(ctx);}
static int reserve_hint(void *ctx,uint32_t records){return cow_store_reserve(ctx,records);}
static void check_file(const char *path,const char *content){
    char b[128]={0};int fd=fs_open(path,FS_O_READ);assert(fd>=0);int n=fs_read(fd,b,sizeof b);
    assert(n==(int)strlen(content)&&!memcmp(b,content,(size_t)n));assert(!fs_close(fd));
}
static int save(const fs_tx_ops_t *ops,void *ctx){
    const char *text=ctx;int fd=ops->open("/home/value",FS_O_WRITE|FS_O_CREATE|FS_O_TRUNC);
    if(fd<0)return fd;
    int n=ops->write(fd,text,strlen(text));
    if(n!=(int)strlen(text))return ERR_IO;
    return ops->close(fd);
}
static jmp_buf abandoned;
static int abandoned_save(const fs_tx_ops_t *ops,void *ctx){assert(!save(ops,ctx));longjmp(abandoned,1);}
static int abort_save(const fs_tx_ops_t *ops,void *ctx){assert(!save(ops,ctx));return ERR_INVAL;}
static int ignore_error(const fs_tx_ops_t *ops,void *ctx){assert(!save(ops,ctx));assert(ops->mkdir("/home")==ERR_EXIST);return 0;}
static int short_save(const fs_tx_ops_t *ops,void *ctx){(void)ctx;int fd=ops->open("/home/huge",FS_O_WRITE|FS_O_CREATE);
    assert(fd>=0);static unsigned char data[SECTORS*512];
    int n=ops->write(fd,data,sizeof data);assert(n>0&&(size_t)n<sizeof data);return 0;}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int pending,attempted,reader_done;
static int delayed_save(const fs_tx_ops_t *ops,void *ctx){
    assert(!save(ops,"pending"));assert(!pthread_mutex_lock(&gate));pending=1;pthread_cond_broadcast(&cond);
    while(!attempted)pthread_cond_wait(&cond,&gate);
    assert(!reader_done);pthread_mutex_unlock(&gate);return save(ops,ctx);
}
static void *reader(void *ctx){(void)ctx;pthread_mutex_lock(&gate);
    while(!pending)pthread_cond_wait(&cond,&gate);
    assert(pthread_mutex_trylock(&lock)==EBUSY);
    attempted=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);
    check_file("/home/value","concurrent");pthread_mutex_lock(&gate);reader_done=1;pthread_mutex_unlock(&gate);return NULL;
}
int main(void){
    assert(fs_selftest());assert(!allocated);
    blockdev_t raw={.block_size=512,.block_count=SECTORS,.read=raw_rd,.write=raw_wr};
    assert(!fs_format(&raw,0,SECTORS));assert(!fs_mount(&raw,0));
    assert(!vfs_mount("/",&vfs_m5fs,NULL));
    size_t confirmed=99;
    assert(!vfs_replace_file("/home/legacy","raw",3,&confirmed)&&confirmed==3);
    check_file("/home/legacy","raw");assert(!fs_delete("/home/legacy"));
    assert(!vfs_umount("/"));
    /* Leave six data sectors free so short-write rollback is exercised before
     * the overlay log runs out of room. This padding lives in the RO base. */
    static unsigned char padding[104*512];
    int pad=fs_open("/home/pad",FS_O_WRITE|FS_O_CREATE);assert(pad>=0);
    assert(fs_write(pad,padding,sizeof padding)==(int)sizeof padding);
    assert(!fs_close(pad));assert(!fs_unmount());
    struct cow_bank io[2];for(int i=0;i<2;i++)io[i]=(struct cow_bank){{&banks[i],CAP,rd,wr,sync_io},reset};
    static struct cow_store store;assert(!cow_store_format(&store,io,42,SECTORS,base,0));
    assert(!cow_store_reserve(&store,8));
    blockdev_t bd;cow_blockdev_init(&bd,&store.view);
    fs_tx_backend_t backend={.ctx=&store,.begin=cow_store_begin,.commit=cow_store_commit,.recover=recover};
    assert(!fs_mount_atomic(&bd,0,&backend));
    assert(!vfs_mount("/",&vfs_m5fs,NULL));
    assert(!fs_transaction(save,"old"));check_file("/home/value","old");
    uint32_t generation=store.view.generation;
    assert(!vfs_replace_file("/home/./value","stable",6,&confirmed)&&confirmed==6);
    assert(store.view.generation==generation+1);check_file("/home/value","stable");
    allowed=APP_PERM_FS_WRITE;
    assert(!vfs_replace_file("/home/value","stable",6,&confirmed)&&confirmed==6);
    allowed=APP_PERM_FS_READ;generation=store.view.generation;confirmed=99;
    assert(vfs_replace_file("/home/value","denied",6,&confirmed)==ERR_PERM&&confirmed==0);
    assert(store.view.generation==generation);allowed|=APP_PERM_FS_WRITE;
    check_file("/home/value","stable");
    budget=13;assert(vfs_replace_file("/home/value","torn",4,&confirmed)<0&&confirmed==0);
    check_file("/home/value","stable");assert(!vfs_count_owned(0));
    static unsigned char too_large[SECTORS*512];
    assert(vfs_replace_file("/home/value",too_large,sizeof too_large,&confirmed)==ERR_NOSPC&&confirmed==0);
    check_file("/home/value","stable");
    assert(vfs_replace_file("/home/absent",too_large,sizeof too_large,&confirmed)==ERR_NOSPC);
    fs_info_t absent;assert(fs_stat("/home/absent",&absent)==ERR_NOENT);
    fail_sync=syncs+1;assert(vfs_replace_file("/home/value","uncommitted",11,&confirmed)<0&&confirmed==0);
    check_file("/home/value","stable");
    fail_sync=syncs+2;assert(vfs_replace_file("/home/value","complete",8,&confirmed)<0&&confirmed==0);
    check_file("/home/value","complete");assert(!vfs_replace_file("/home/value",NULL,0,&confirmed)&&confirmed==0);
    check_file("/home/value","");assert(!vfs_replace_file("/home/value","old",3,NULL));
    assert(!vfs_count_owned(0));
    /* Owner termination inside the real replace write must release the VFS
     * pin without closing a nonexistent file, then recover M5FS before reading. */
    if(!setjmp(replacement_abandoned)){
        abandon_write=true;
        (void)vfs_replace_file("/home/value","abandoned replacement",21,&confirmed);
        assert(0);
    }
    mutex_unlock(NULL); /* kernel releases the terminated owner's mutex */
    assert(vfs_close_owned(0)==1);assert(!vfs_count_owned(0));
    check_file("/home/value","old");
    /* Near a log boundary, the save's size hint triggers collection BEFORE
     * truncation instead of failing halfway through a perfectly valid save. */
    while ((store.view.log.capacity-store.view.end)/COW_RECORD >= 20)
        assert(!fs_transaction(save,"old"));
    assert(!fs_unmount());backend.reserve=reserve_hint;
    assert(!fs_mount_atomic(&bd,0,&backend));
    uint64_t epoch=store.epoch;
    assert(!vfs_replace_file("/home/value","old",3,&confirmed));
    assert(store.epoch>epoch);check_file("/home/value","old");
    assert(!fs_unmount());backend.reserve=NULL;
    assert(!fs_mount_atomic(&bd,0,&backend));assert(!cow_store_reserve(&store,8));
    int stale=fs_open("/home/value",FS_O_READ);assert(stale>=0);
    assert(fs_transaction(abort_save,"discard")==ERR_INVAL);check_file("/home/value","old");
    int fresh=fs_open("/home/value",FS_O_READ);assert(fresh>=0&&fresh!=stale);
    char b[20];assert(fs_read(stale,b,sizeof b)==ERR_BADF);assert(!fs_close(fresh));
    if (!setjmp(abandoned)) { (void)fs_transaction(abandoned_save,"abandoned"); assert(0); }
    /* Model kernel mutex_release_all after task termination. */
    mutex_unlock(NULL);
    check_file("/home/value","old");
    assert(!fs_transaction(save,"old"));
    assert(fs_transaction(ignore_error,"discard")==ERR_EXIST);check_file("/home/value","old");
    assert(fs_transaction(short_save,NULL)==ERR_NOSPC);fs_info_t info;assert(fs_stat("/home/huge",&info)==ERR_NOENT);
    check_file("/home/value","old");
    budget=13;assert(fs_transaction(save,"torn")<0);check_file("/home/value","old");
    assert(!fs_transaction(save,"after-error"));check_file("/home/value","after-error");
    /* Failure at final sync may have committed. Report failure but expose one
     * authoritative complete generation, invalidate descriptors and continue. */
    fail_sync=syncs+2;assert(fs_transaction(save,"uncertain")<0);check_file("/home/value","uncertain");
    assert(!fs_transaction(save,"continued"));check_file("/home/value","continued");
    pthread_t t;assert(!pthread_create(&t,NULL,reader,NULL));assert(!fs_transaction(delayed_save,"concurrent"));
    assert(!pthread_join(t,NULL));assert(reader_done);
    for(int i=0;i<25;i++){assert(!fs_transaction(save,"repeat"));check_file("/home/value","repeat");}
    int handles[8];for(int i=0;i<8;i++){handles[i]=fs_open("/home/value",FS_O_READ);assert(handles[i]>=0);}
    assert(fs_open("/home/no-slot",FS_O_CREATE|FS_O_WRITE)==ERR_BUSY);
    assert(fs_stat("/home/no-slot",&info)==ERR_NOENT);check_file("/home/value","repeat");
    for(int i=0;i<8;i++)assert(fs_read(handles[i],b,sizeof b)==ERR_BADF);
    /* Normal APIs commit individually. */
    int fd=fs_open("/home/api",FS_O_WRITE|FS_O_CREATE);assert(fd>=0);
    assert(fs_write(fd,"api",3)==3);assert(!fs_close(fd));check_file("/home/api","api");
    assert(!fs_unmount());assert(!fs_mount_readonly(&bd,0));
    assert(fs_open("/home/value",FS_O_WRITE|FS_O_TRUNC)==ERR_PERM);
    assert(fs_mkdir("/home/no")==ERR_PERM);
    assert(vfs_replace_file("/home/value","no",2,&confirmed)==ERR_PERM&&confirmed==0);
    check_file("/home/value","repeat");
    assert(!fs_sync());assert(!fs_unmount());assert(!allocated);
    assert(!fs_mount_atomic(&bd,0,&backend));fail_recover=1;
    assert(fs_transaction(abort_save,"unrecoverable")==ERR_IO);
    assert(fs_stat("/home/value",&info)<0);assert(!allocated);
    fail_recover=0;assert(!cow_store_abort(&store));
    assert(!fs_mount_atomic(&bd,0,&backend));check_file("/home/value","repeat");
    int before_sync=syncs;
    assert(!fs_discard());assert(syncs==before_sync);assert(!allocated);
    assert(!vfs_umount("/"));
    puts("M5FS/VFS ATOMIC SAVE PASS: permissions, single commit, old-file rollback, empty replace, capacity, pin cleanup; legacy selftest, grouped save, rollback, sticky errors, short write, stale descriptors, torn IO, uncertain commit, continued saves, abandoned owner recovery, serialized reader, failed recovery stays offline, RO teardown, no heap leaks");
}
