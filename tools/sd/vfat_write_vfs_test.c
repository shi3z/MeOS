#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m5os/fat.h>
#include <m5os/errno.h>
#include <m5os/mutex.h>
#include <m5os/task.h>
#include <m5os/app.h>
#include "../../user/shell/sdbench.h"
static unsigned char *image;
static size_t size;
static int depth;
void mutex_lock(mutex_t *m){(void)m;assert(!depth);depth++;}
void mutex_unlock(mutex_t *m){(void)m;assert(depth==1);depth--;}
void *kmalloc(size_t n){return malloc(n);}
void *kmalloc_psram(size_t n){return malloc(n);}
static uint64_t fake_time;
static bool abort_bench,corrupt_payload;
uint64_t hal_time_now_us(void){fake_time+=10;return fake_time;}
void task_sleep(uint32_t ms){(void)ms;}
int kconsole_getc(void){return abort_bench?3:-1;}
int ksnprintf(char *out,size_t n,const char *fmt,...){va_list ap;va_start(ap,fmt);int rc=vsnprintf(out,n,fmt,ap);va_end(ap);return rc;}
void kfree(void *p){free(p);}
bool task_has_perm(uint32_t p){(void)p;return true;}
task_t *task_current(void){return NULL;}
uint32_t arch_irq_disable(void){return 0;}
void arch_irq_restore(uint32_t s){(void)s;}
void kprintf(const char *fmt,...){(void)fmt;}
const vfs_fs_t vfs_devfs={.name="test-dev"};
static int read_blocks(blockdev_t *bd,uint32_t lba,uint32_t n,void *out){
    assert(depth && lba<bd->block_count && n<=bd->block_count-lba);
    memcpy(out,image+(size_t)lba*512,(size_t)n*512);
    if(corrupt_payload) {
        bool match=true;
        for(unsigned i=0;i<8;i++) {
            uint32_t v=(i*1664525u+1013904223u)^((i>>8)*2246822519u);
            if(((uint8_t *)out)[i]!=(uint8_t)(v^(v>>16)^(v>>24)))match=false;
        }
        if(match)((uint8_t *)out)[0]^=1;
    }
    return 0;
}
static int write_blocks(blockdev_t *bd,uint32_t lba,uint32_t n,const void *in){
    assert(depth && n && lba<bd->block_count && n<=bd->block_count-lba);
    memcpy(image+(size_t)lba*512,in,(size_t)n*512);return 0;
}
int main(int argc,char **argv){
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);size=(size_t)ftell(f);rewind(f);
    image=malloc(size);assert(image && fread(image,1,size,f)==size);fclose(f);
    blockdev_t bd={.block_size=512,.block_count=(uint32_t)(size/512),.read=read_blocks,.write=write_blocks};
    vfs_init();assert(!fat_mount_readonly(&bd));
    assert(sd_benchmark(4,1024,1)==ERR_PERM);
    int fd=vfs_open("/sd/HELLO.TXT",VFS_O_READ);assert(fd>=0);assert(!vfs_close(fd));
    assert(!fat_unmount());assert(!fat_mount(&bd,false));
    const char *dir="/sd/\u4fdd\u5b58\u8a66\u9a13";
    const char *path="/sd/\u4fdd\u5b58\u8a66\u9a13/\u65e5\u672c\u8a9e\U00020bb7.txt";
    assert(!vfs_mkdir(dir));
    fd=vfs_open(path,VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC);assert(fd>=0);
    const char text[]="UTF-8: \u65e5\u672c\u8a9e\n";
    assert(vfs_write(fd,text,sizeof text-1)==sizeof text-1);assert(!vfs_close(fd));
    assert(!fat_unmount());assert(!fat_mount(&bd,false));
    fd=vfs_open(path,VFS_O_READ);assert(fd>=0);char contents[100];
    assert(vfs_read(fd,contents,sizeof contents)==sizeof text-1 && !memcmp(contents,text,sizeof text-1));
    assert(!vfs_close(fd));assert(!vfs_unlink(path));assert(!vfs_unlink(dir));
    assert(sd_benchmark(0,1024,1)==ERR_INVAL);
    abort_bench=true;assert(sd_benchmark(4,1024,1)==ERR_BUSY);abort_bench=false;
    corrupt_payload=true;assert(sd_benchmark(1,1024,1)==ERR_CORRUPT);corrupt_payload=false;
    assert(!sd_benchmark(4,1024,1));
    assert(!fat_unmount());free(image);
    puts("VFAT read/write through real VFS: UTF-8 create/save/remount/read/delete PASS");
}
