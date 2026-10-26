#include "checksum.h"
#include <m5os/crc32.h>
#include <m5os/errno.h>
#include <m5os/hal_time.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/vfs.h>

int shell_crc32_file(const char *path)
{
    vfs_info_t info;
    int rc=vfs_stat(path,&info);
    if(rc<0)return rc;
    if(info.type!=FS_TYPE_FILE)return info.type==FS_TYPE_DIR?ERR_ISDIR:ERR_INVAL;
    int fd=vfs_open(path,VFS_O_READ);
    if(fd<0)return fd;
    uint8_t block[4096];uint32_t bytes=0,sum=0,since_nap=0;
    uint64_t started=hal_time_now_us();
    for(;;){
        int n=vfs_read(fd,block,sizeof block);
        if(n<0){rc=n;break;}
        if(!n){rc=0;break;}
        if((size_t)n>sizeof block||(uint32_t)n>UINT32_MAX-bytes){rc=ERR_IO;break;}
        sum=crc32_update(sum,block,(size_t)n);bytes+=(uint32_t)n;since_nap+=(uint32_t)n;
        /* Yield alone does not schedule lower-priority network services. */
        if(since_nap>=64u*1024u){task_sleep(1);since_nap=0;}
    }
    int closed=vfs_close(fd);
    if(rc<0)return rc;
    if(closed<0)return closed;
    kprintf("CRC32 %08x %u bytes %u ms %s\n",sum,bytes,
            (uint32_t)((hal_time_now_us()-started)/1000u),path);
    return 0;
}
