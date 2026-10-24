#include <m5os/fs.h>
#include <m5os/errno.h>
#include <m5os/mutex.h>
#include "../../fs/m5fs/m5fs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BLOCKS 128u
static unsigned char disk[BLOCKS*512],saved[BLOCKS*512];
static unsigned allocations;
void *kmalloc(size_t n){void *p=malloc(n);if(p)allocations++;return p;}
void *kmalloc_psram(size_t n){return kmalloc(n);}
void *kcalloc(size_t n,size_t z){void *p=calloc(n,z);if(p)allocations++;return p;}
void kfree(void *p){if(p){assert(allocations);allocations--;free(p);}}
void mutex_lock(mutex_t *m){assert(!m->owner);m->owner=(void *)1;}
void mutex_unlock(mutex_t *m){assert(m->owner);m->owner=NULL;}
static int read_blocks(blockdev_t *d,uint32_t l,uint32_t n,void *p){(void)d;assert(l<=BLOCKS&&n<=BLOCKS-l);memcpy(p,disk+l*512,n*512);return 0;}
static int write_blocks(blockdev_t *d,uint32_t l,uint32_t n,const void *p){(void)d;assert(l<=BLOCKS&&n<=BLOCKS-l);memcpy(disk+l*512,p,n*512);return 0;}
static void extent(const m5fs_super_t *sb,uint32_t start,uint32_t count){
    /* /home/probe is the first file after the three standard directories. */
    size_t pos=(size_t)sb->dir_start*512+3*M5FS_DIRENT_SIZE;
    m5fs_dirent_t e;memcpy(&e,disk+pos,sizeof e);assert(e.type==M5FS_T_FILE);
    e.extent[0]=(m5fs_extent_t){start,count};memcpy(disk+pos,&e,sizeof e);
}
int main(void){
    blockdev_t bd={.block_size=512,.block_count=BLOCKS,.read=read_blocks,.write=write_blocks};
    assert(!fs_format(&bd,0,BLOCKS));assert(!fs_mount(&bd,0));
    int fd=fs_open("/home/probe",FS_O_CREATE|FS_O_WRITE);assert(fd>=0);
    assert(fs_write(fd,"x",1)==1);assert(!fs_close(fd));assert(!fs_unmount());assert(!allocations);
    memcpy(saved,disk,sizeof disk);m5fs_super_t sb;memcpy(&sb,disk,sizeof sb);
    const m5fs_extent_t bad[]={
        {UINT32_MAX,2}, {UINT32_MAX-1,4}, {sb.data_start,UINT32_MAX},
        {BLOCKS,1}, {BLOCKS-1,2}, {sb.data_start-1,1}
    };
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){
        memcpy(disk,saved,sizeof disk);extent(&sb,bad[i].start,bad[i].count);
        assert(fs_mount(&bd,0)==ERR_CORRUPT);assert(!allocations);
    }
    memcpy(disk,saved,sizeof disk);assert(!fs_mount(&bd,0));assert(!fs_unmount());
    /* An extent ending exactly at the volume boundary remains legal. */
    memcpy(disk,saved,sizeof disk);extent(&sb,BLOCKS-1,1);
    unsigned char *bitmap=disk+sb.bitmap_start*512;
    bitmap[sb.data_start/8]&=(unsigned char)~(1u<<(sb.data_start%8));
    bitmap[(BLOCKS-1)/8]|=(unsigned char)(1u<<((BLOCKS-1)%8));
    assert(!fs_mount(&bd,0));assert(!fs_unmount());assert(!allocations);
    puts("M5FS mount: wrapped/out-of-range extents rejected, exact-boundary extent accepted, no allocation leaks PASS");
}
