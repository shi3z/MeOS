#define _DEFAULT_SOURCE
#include "../../fs/cow/blockdev.h"
#include <m5os/fs.h>
#include <m5os/vfs.h>
#include <m5os/app.h>
#include <m5os/kmalloc.h>
#include <m5os/mutex.h>
#include <m5os/errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
static uint32_t allowed = APP_PERM_FS_READ | APP_PERM_FS_WRITE;
bool task_has_perm(uint32_t p){return (allowed&p)==p;}
task_t *task_current(void){return NULL;}
uint32_t arch_irq_disable(void){return 0;}
void arch_irq_restore(uint32_t flags){(void)flags;}
const vfs_fs_t vfs_devfs={.name="test-dev"};
void kprintf(const char *fmt,...){(void)fmt;}
#define SECTORS 128u
#define CAP (96u*COW_RECORD)
static unsigned char base_image[SECTORS*512];
static unsigned char old_image[SECTORS*512],new_image[SECTORS*512];
static unsigned char *disk;
static int budget;
static const fs_tx_ops_t *tx;
static int call_open(const char *p,uint32_t f){return tx?tx->open(p,f):fs_open(p,f);}
static int call_close(int fd){return tx?tx->close(fd):fs_close(fd);}
static int call_write(int fd,const void *p,size_t n){return tx?tx->write(fd,p,n):fs_write(fd,p,n);}
static int call_mkdir(const char *p){return tx?tx->mkdir(p):fs_mkdir(p);}
static int call_delete(const char *p){return tx?tx->unlink(p):fs_delete(p);}
void *kmalloc(size_t n){return malloc(n);}
void *kmalloc_psram(size_t n){return malloc(n);}
void *kcalloc(size_t n,size_t s){return calloc(n,s);}
void kfree(void *p){free(p);}
void mutex_lock(mutex_t *m){assert(!m->owner);m->owner=(void *)1;}
void mutex_unlock(mutex_t *m){assert(m->owner);m->owner=0;}
static int base_read(void *ctx,uint32_t lba,void *p){(void)ctx;assert(lba<SECTORS);memcpy(p,base_image+lba*512,512);return 0;}
static int raw_read(blockdev_t *b,uint32_t l,uint32_t n,void *p){(void)b;assert(l+n<=SECTORS);memcpy(p,base_image+l*512,n*512);return 0;}
static int raw_write(blockdev_t *b,uint32_t l,uint32_t n,const void *p){(void)b;assert(l+n<=SECTORS);memcpy(base_image+l*512,p,n*512);return 0;}
static int rd(void *ctx,uint32_t o,void *p,size_t n){(void)ctx;assert(o+n<=CAP);memcpy(p,disk+o,n);return 0;}
static int wr(void *ctx,uint32_t o,const void *p,size_t n){
    (void)ctx;assert(o+n<=CAP);size_t take=n;
    if(budget>=0&&(size_t)budget<take)take=(size_t)budget;
    memcpy(disk+o,p,take);if(budget>=0)budget-=(int)take;
    if(take!=n)_exit(90);
    return 0;
}
static int sync_io(void *ctx){(void)ctx;return 0;}
static struct cow_io io={0,CAP,rd,wr,sync_io};
static void mount_overlay(struct cow *c,blockdev_t *bd){assert(!cow_mount(c,io,42,SECTORS,base_read,0));cow_blockdev_init(bd,c);}
static void write_file(const char *path,int value,size_t n,unsigned flags){
    unsigned char b[1200];memset(b,value,sizeof b);assert(n<=sizeof b);
    int fd=call_open(path,FS_O_WRITE|flags);assert(fd>=0);
    assert(call_write(fd,b,n)==(int)n);assert(!call_close(fd));
}
static void mutation(int op){
    switch(op){
    case 0:write_file("/home/new",22,900,FS_O_CREATE);break;
    case 1:write_file("/home/seed",22,900,FS_O_APPEND);break;
    case 2:write_file("/home/seed",22,111,FS_O_TRUNC);break;
    case 3:assert(!call_delete("/home/seed"));break;
    case 4:assert(!call_mkdir("/home/sub"));break;
    case 5:{
        int a=call_open("/home/a",FS_O_WRITE|FS_O_CREATE),b=call_open("/home/b",FS_O_WRITE|FS_O_CREATE);
        assert(a>=0&&b>=0);assert(call_write(a,"aaa",3)==3);assert(call_write(b,"bbb",3)==3);
        assert(!call_close(a));assert(!call_close(b));break;
    }
    default:abort();
    }
}
static int execute(const fs_tx_ops_t *ops,void *ctx){tx=ops;mutation(*(int *)ctx);tx=NULL;return 0;}
static int begin(void *ctx){return cow_begin(ctx);}
static int commit(void *ctx){return cow_commit(ctx);}
static int recover_unused(void *ctx){(void)ctx;return -1;}
static void run_child(int op,int cut){
    pid_t pid=fork();assert(pid>=0);
    if(!pid){
        struct cow c;blockdev_t bd;budget=cut;mount_overlay(&c,&bd);
        fs_tx_backend_t backend={.ctx=&c,.begin=begin,.commit=commit,.recover=recover_unused};
        assert(!fs_mount_atomic(&bd,0,&backend));
        if(op<6)assert(!fs_transaction(execute,&op));
        else {
            assert(!vfs_mount("/",&vfs_m5fs,NULL));
            unsigned char payload[32768];memset(payload,22,sizeof payload);
            const char *path=op<8?"/home/seed":"/home/new";
            size_t len=op==6?16384:op==9?sizeof payload:0,confirmed=99;
            assert(!vfs_replace_file(path,payload,len,&confirmed)&&confirmed==len);
            assert(!vfs_count_owned(0));
        }
        _exit(0);
    }
    int status;assert(waitpid(pid,&status,0)==pid);assert(WIFEXITED(status));
    assert(WEXITSTATUS(status)==0||WEXITSTATUS(status)==90);
}
static void verify(const unsigned char *expected){
    /* New process models power-on: no dirty M5FS globals survive. */
    pid_t pid=fork();assert(pid>=0);
    if(!pid){
        struct cow c;blockdev_t bd;unsigned char b[512];mount_overlay(&c,&bd);
        for(unsigned i=0;i<SECTORS;i++){assert(!cow_read(&c,i,b));assert(!memcmp(b,expected+i*512,512));}
        assert(!fs_mount_readonly(&bd,0));fs_info_t info;assert(!fs_stat("/home",&info));
        fs_stat_t st;assert(!fs_statfs(&st));assert(st.total_blocks==SECTORS);
        assert(!fs_unmount());_exit(0);
    }
    int status;assert(waitpid(pid,&status,0)==pid);assert(WIFEXITED(status)&&!WEXITSTATUS(status));
}
int main(void){
    disk=mmap(0,CAP,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANON,-1,0);assert(disk!=MAP_FAILED);
    blockdev_t raw={.block_size=512,.block_count=SECTORS,.read=raw_read,.write=raw_write};
    assert(!fs_format(&raw,0,SECTORS));assert(!fs_mount(&raw,0));
    write_file("/home/seed",11,600,FS_O_CREATE);assert(!fs_unmount());
    memcpy(old_image,base_image,sizeof old_image);unsigned cases=0;
    for(int op=0;op<10;op++){
        memset(disk,0,CAP);run_child(op,-1);struct cow c;blockdev_t bd;mount_overlay(&c,&bd);
        uint32_t bytes=c.committed;assert(bytes>0&&c.generation==1);
        for(unsigned i=0;i<SECTORS;i++)assert(!cow_read(&c,i,new_image+i*512));
        verify(new_image);
        const unsigned offsets[]={0,1,31,32,256,548,551};
        for(uint32_t r=0;r<bytes/COW_RECORD;r++)for(unsigned j=0;j<sizeof offsets/sizeof offsets[0];j++){
            memset(disk,0,CAP);run_child(op,(int)(r*COW_RECORD+offsets[j]));
            mount_overlay(&c,&bd);verify(c.generation?new_image:old_image);
            assert(!memcmp(base_image,old_image,sizeof old_image));cases++;
        }
    }
    assert(!munmap(disk,CAP));printf("M5FS COW PASS: %u crash cases; create/grow/truncate/delete/mkdir/multiple-open/VFS-whole-file-save; full recovered block image old-or-new\n",cases);
}
