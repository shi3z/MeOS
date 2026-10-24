#include <m5os/types.h>
#include <m5os/vfs.h>
#include <m5os/errno.h>
#include <m5os/crc32.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static uint8_t input[5000],output[5000];
static unsigned available,pos,written,opens,removed,closed;
static bool exists,write_error,close_error;
static uint64_t now;
int quake_receive(int argc,char **argv);
static bool native;
const char *quake_storage_base(void){return native?"/assets/quake":"/sd";}
static const char *destination(void){return native?"/assets/quake/id1/pak0.pak":"/sd/id1/pak0.pak";}
int vfs_mkdir(const char *p){(void)p;return 0;}
int ksnprintf(char *buf,size_t n,const char *fmt,...){va_list ap;va_start(ap,fmt);int rc=vsnprintf(buf,n,fmt,ap);va_end(ap);return rc;}
uint64_t hal_time_now_us(void){now+=1000000;return now;}
void task_yield(void){}
int kconsole_getc(void){return pos<available?input[pos++]:-1;}
void kputs(const char *s){(void)s;}
void kprintf(const char *s,...){(void)s;}
int vfs_stat(const char *p,vfs_info_t *out){(void)out;assert(!strcmp(p,destination()));return exists?0:ERR_NOENT;}
int vfs_open(const char *p,uint32_t flags){assert(!strcmp(p,destination())&&!exists);assert(flags==(VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC));opens++;exists=true;return 4;}
int vfs_write(int fd,const void *p,size_t n){assert(fd==4&&written+n<=sizeof output);if(write_error)return -1;memcpy(output+written,p,n);written+=n;return (int)n;}
int vfs_close(int fd){assert(fd==4);closed++;return close_error?-1:0;}
int vfs_unlink(const char *p){assert(!strcmp(p,destination())&&exists);removed++;exists=false;return 0;}
static int run(unsigned n,unsigned bytes,uint32_t crc){available=bytes;pos=written=opens=removed=closed=0;now=0;char size[20],sum[20];snprintf(size,sizeof size,"%u",n);snprintf(sum,sizeof sum,"%08x",crc);char *args[]={"quake","receive",size,sum};return quake_receive(4,args);}
int main(void){
 for(unsigned i=0;i<sizeof input;i++)input[i]=(uint8_t)(i*7);
 uint32_t sum=crc32(input,sizeof input);
 assert(!run(sizeof input,sizeof input,sum));assert(exists&&written==sizeof input&&!memcmp(output,input,sizeof input)&&closed==1&&!removed);
 assert(run(sizeof input,sizeof input,sum)&&!opens&&!removed);exists=false;
 assert(run(sizeof input,sizeof input,sum^1)&&removed==1&&!exists&&closed==1);
 assert(run(sizeof input,11,sum)&&removed==1&&!exists&&closed==1);
 write_error=true;assert(run(sizeof input,sizeof input,sum)&&removed==1&&!exists&&closed==1);write_error=false;
 close_error=true;assert(run(sizeof input,sizeof input,sum)&&removed==1&&!exists&&closed==1);close_error=false;
 assert(run(0,0,0)&&!opens);assert(run(64u*1024*1024+1,0,0)&&!opens);
 native=true;assert(!run(sizeof input,sizeof input,sum)&&exists);exists=false;
 puts("Quake transfer: no overwrite, two-block CRC, timeout/write/close failure cleanup and bounds PASS");
}
