#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <m5os/crc32.h>
#include <m5os/errno.h>
#include <m5os/vfs.h>
#include "../../user/shell/checksum.h"

static unsigned char data[256*1024];
static size_t length,position,chunk;
static int stat_error,open_error,read_error,close_error,closed,sleeps;
static fs_type_t type;
static bool overread;
static char receipt[256];
int vfs_stat(const char *path,vfs_info_t *info){assert(!strcmp(path,"/asset"));*info=(vfs_info_t){.type=type,.size=(uint32_t)length};return stat_error;}
int vfs_open(const char *path,uint32_t flags){assert(!strcmp(path,"/asset")&&flags==VFS_O_READ);return open_error?open_error:3;}
int vfs_read(int fd,void *buf,size_t size){
    assert(fd==3);
    if(read_error)return read_error;
    if(overread)return (int)size+1;
    size_t n=length-position;if(n>size)n=size;if(n>chunk)n=chunk;
    memcpy(buf,data+position,n);position+=n;return (int)n;
}
int vfs_close(int fd){assert(fd==3);closed++;return close_error;}
void task_sleep(uint32_t ms){assert(ms==1);sleeps++;}
uint64_t hal_time_now_us(void){static uint64_t now;return now+=1000;}
void kprintf(const char *fmt,...){va_list ap;va_start(ap,fmt);vsnprintf(receipt,sizeof receipt,fmt,ap);va_end(ap);}
static void reset(size_t size){
    length=size;position=0;chunk=4096;stat_error=open_error=read_error=close_error=closed=sleeps=0;
    type=FS_TYPE_FILE;overread=false;receipt[0]=0;
}
int main(void){
    reset(0);assert(!shell_crc32_file("/asset")&&closed==1&&!sleeps);
    assert(strstr(receipt,"CRC32 00000000 0 bytes"));
    memcpy(data,"123456789",9);reset(9);chunk=2;
    assert(!shell_crc32_file("/asset")&&closed==1&&position==9);
    assert(strstr(receipt,"CRC32 cbf43926 9 bytes"));
    for(size_t i=0;i<sizeof data;i++)data[i]=(unsigned char)(i*29u+(i>>9));
    reset(sizeof data);chunk=997;uint32_t sum=crc32(data,sizeof data);
    assert(!shell_crc32_file("/asset")&&closed==1&&sleeps>=3&&sleeps<=4);
    char expected[32];snprintf(expected,sizeof expected,"CRC32 %08x 262144 bytes",sum);
    assert(strstr(receipt,expected));
    reset(9);stat_error=ERR_NOENT;assert(shell_crc32_file("/asset")==ERR_NOENT&&!closed&&!receipt[0]);
    reset(9);type=FS_TYPE_DIR;assert(shell_crc32_file("/asset")==ERR_ISDIR&&!closed);
    reset(9);type=FS_TYPE_DEV;assert(shell_crc32_file("/asset")==ERR_INVAL&&!closed);
    reset(9);open_error=ERR_PERM;assert(shell_crc32_file("/asset")==ERR_PERM&&!closed);
    reset(9);read_error=ERR_IO;close_error=ERR_BUSY;
    assert(shell_crc32_file("/asset")==ERR_IO&&closed==1&&!receipt[0]);
    reset(9);close_error=ERR_IO;assert(shell_crc32_file("/asset")==ERR_IO&&closed==1&&!receipt[0]);
    reset(9);overread=true;assert(shell_crc32_file("/asset")==ERR_IO&&closed==1&&!receipt[0]);
    puts("CRC32 command PASS: vectors, short reads, bounded scheduling, regular-file-only, read/close errors");
}
