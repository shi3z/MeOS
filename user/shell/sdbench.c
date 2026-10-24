#include "sdbench.h"
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/fat.h>
#include <m5os/errno.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <string.h>
#define CHUNK 4096u
#define PHASE_LIMIT_US (600ull*1000000ull)
typedef struct { uint8_t *buffer;uint64_t start,progress;const char *phase;uint32_t done; } bench;
static uint8_t pattern(unsigned seed,uint32_t offset)
{
    uint32_t v=(offset*1664525u+1013904223u)^((offset>>8)*2246822519u)^(seed*2654435761u);
    return (uint8_t)(v^(v>>16)^(v>>24));
}
static void start(bench *b,const char *phase)
{
    b->phase=phase;kprintf("SDBENCH start=%s\n",phase);
    b->done=0;b->start=b->progress=hal_time_now_us();
}
static int tick(bench *b,uint32_t done)
{
    task_sleep(0);
    if(kconsole_getc()==3)return ERR_BUSY;
    uint64_t now=hal_time_now_us();
    if(now-b->start>PHASE_LIMIT_US)return ERR_TIMEOUT;
    if(now-b->progress>=2000000) {kprintf("SDBENCH progress=%s done=%u\n",b->phase,done);b->progress=now;}
    return 0;
}
static void finish(bench *b,unsigned files,uint32_t bytes)
{
    uint64_t us=hal_time_now_us()-b->start;if(!us)us=1;
    uint64_t rate=(uint64_t)bytes*10000000/(1024*us);
    uint64_t fps=(uint64_t)files*10000000/us;
    kprintf("SDBENCH phase=%s files=%u bytes=%u ms=%u KiB_s=%u.%u files_s=%u.%u verified=%u\n",
        b->phase,files,bytes,(unsigned)(us/1000),(unsigned)(rate/10),(unsigned)(rate%10),
        (unsigned)(fps/10),(unsigned)(fps%10),strstr(b->phase,"read")?1u:0u);
}
static int transfer(bench *b,const char *path,uint32_t bytes,unsigned seed,bool write)
{
    int fd=vfs_open(path,write?VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC:VFS_O_READ);
    if(fd<0)return fd;
    int rc=0;uint32_t pos=0;
    while(pos<bytes) {
        unsigned n=bytes-pos;if(n>CHUNK)n=CHUNK;
        if(write)for(unsigned i=0;i<n;i++)b->buffer[i]=pattern(seed,pos+i);
        int got=write?vfs_write(fd,b->buffer,n):vfs_read(fd,b->buffer,n);
        if(got!=(int)n){rc=got<0?got:write?ERR_NOSPC:ERR_IO;break;}
        if(!write)for(unsigned i=0;i<n;i++)if(b->buffer[i]!=pattern(seed,pos+i)){rc=ERR_CORRUPT;break;}
        if(rc)break;
        pos+=n;b->done+=n;rc=tick(b,b->done);if(rc)break;
    }
    if(!rc && !write){uint8_t extra;int n=vfs_read(fd,&extra,1);if(n!=0)rc=n<0?n:ERR_CORRUPT;}
    int closed=vfs_close(fd);return rc?rc:closed;
}
int sd_benchmark(unsigned files,unsigned small_bytes,unsigned large_mib)
{
    if(files<1 || files>512 || small_bytes<1 || small_bytes>65536 || large_mib<1 || large_mib>64)return ERR_INVAL;
    if(!fat_is_mounted())return ERR_NODEV;
    if(fat_is_readonly())return ERR_PERM;
    bench b={0};b.buffer=kmalloc_psram(CHUNK);if(!b.buffer)return ERR_NOMEM;
    char dir[80],path[112];int rc=ERR_EXIST;unsigned tag=(unsigned)hal_time_now_us();
    for(unsigned i=0;i<100 && rc==ERR_EXIST;i++){
        ksnprintf(dir,sizeof dir,"/sd/MEOSBENCH-%08x-%02u",tag,i);rc=vfs_mkdir(dir);
    }
    if(rc){kfree(b.buffer);return rc;}
    kprintf("SDBENCH dir=%s files=%u small_bytes=%u large_MiB=%u chunk=%u write_through=1\n",dir,files,small_bytes,large_mib,CHUNK);
    kprintf("SDBENCH Ctrl-C aborts; errors preserve this directory. Existing files are untouched.\n");
    start(&b,"small-write");
    for(unsigned i=0;i<files;i++){
        ksnprintf(path,sizeof path,"%s/small-file-%04u.dat",dir,i);rc=transfer(&b,path,small_bytes,i,true);if(rc)goto failed;
    }
    finish(&b,files,files*small_bytes);
    start(&b,"small-read");
    for(unsigned i=0;i<files;i++){
        ksnprintf(path,sizeof path,"%s/small-file-%04u.dat",dir,i);rc=transfer(&b,path,small_bytes,i,false);if(rc)goto failed;
    }
    finish(&b,files,files*small_bytes);
    ksnprintf(path,sizeof path,"%s/LARGE.DAT",dir);uint32_t bytes=large_mib*1024u*1024u;
    start(&b,"large-write");rc=transfer(&b,path,bytes,0x1234,true);if(rc)goto failed;finish(&b,1,bytes);
    start(&b,"large-read");rc=transfer(&b,path,bytes,0x1234,false);if(rc)goto failed;finish(&b,1,bytes);
    start(&b,"small-delete");
    for(unsigned i=0;i<files;i++){
        ksnprintf(path,sizeof path,"%s/small-file-%04u.dat",dir,i);rc=vfs_unlink(path);if(rc)goto failed;
        rc=tick(&b,i+1);if(rc)goto failed;
    }
    finish(&b,files,0);
    ksnprintf(path,sizeof path,"%s/LARGE.DAT",dir);rc=vfs_unlink(path);if(rc)goto failed;
    rc=vfs_unlink(dir);if(rc)goto failed;
    kfree(b.buffer);kprintf("SDBENCH PASS verified=1 cleaned=1 dir=%s\n",dir);return 0;
failed:
    kprintf("SDBENCH FAIL phase=%s error=%d retained=%s\n",b.phase,rc,dir);
    kfree(b.buffer);return rc;
}
#endif
