#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <m5os/vfs.h>
#include <m5os/types.h>
#define QUAKE_COMPAT_DECLARATIONS_ONLY
#include "../../components/quakeengine/compat.h"
uint32_t quake_owner=17;
char quake_basedir[96]="/sd";
static const char payload[]="PACK0123456789";
static struct {int open,pos;} slots[16];
static int closes,fail_reads;
void *kmalloc_psram_owned(size_t n,uint32_t owner){assert(owner==17);return malloc(n);}
void kfree(void *p){free(p);}
void kputs(const char *s){(void)s;}
void kprintf(const char *s,...){(void)s;}
void quake_exit(int n){(void)n;abort();}
int vfs_normalize(const char *p,char *out){if(strstr(p,".."))strcpy(out,"/secret");else strcpy(out,p);return 0;}
int vfs_stat(const char *p,vfs_info_t *i){assert(!strncmp(p,"/sd/",4)||!strncmp(p,"/assets/quake/",14));*i=(vfs_info_t){.type=FS_TYPE_FILE,.size=sizeof payload-1};return 0;}
int vfs_open(const char *p,uint32_t flags){assert((!strncmp(p,"/sd/",4)||!strncmp(p,"/assets/quake/",14))&&flags==VFS_O_READ);for(int i=0;i<16;i++)if(!slots[i].open){slots[i].open=1;slots[i].pos=0;return i;}return -1;}
int vfs_close(int fd){assert(slots[fd].open);slots[fd].open=0;closes++;return 0;}
int vfs_read(int fd,void *p,size_t n){assert(slots[fd].open);if(fail_reads)return -1;size_t left=sizeof payload-1-slots[fd].pos;if(n>left)n=left;memcpy(p,payload+slots[fd].pos,n);slots[fd].pos+=(int)n;return (int)n;}
int vfs_seek(int fd,int32_t off,int whence){int64_t next=whence==SEEK_SET?off:whence==SEEK_CUR?(int64_t)slots[fd].pos+off:whence==SEEK_END?(int64_t)sizeof payload-1+off:-1;if(next<0||next>(int64_t)sizeof payload-1)return -1;slots[fd].pos=(int)next;return (int)next;}
extern void quake_io_close_all(void);
int main(void){
 assert(!quake_fopen("/sd/id1/s0.sav","r"));
 assert(!quake_fopen("/sd/id1/pak0.pak","wb"));assert(!quake_fopen("/etc/config","rb"));assert(!quake_fopen("/sd/../secret","r"));
 FILE *f=quake_fopen("/sd/id1/pak0.pak","rb");assert(f);char b[30]={0};
 assert(quake_fread(b,1,4,f)==4&&!memcmp(b,"PACK",4));assert(quake_ftell(f)==4);
 assert(!quake_fseek(f,-2,SEEK_END));assert(quake_fread(b,1,sizeof b,f)==2);assert(quake_feof(f));
 assert(quake_fseek(f,-1,SEEK_SET)<0&&quake_ferror(f));assert(!quake_fseek(f,0,SEEK_SET));
 assert(!quake_fread(b,SIZE_MAX,2,f));assert(!quake_fwrite(b,1,1,f));fail_reads=1;assert(!quake_fread(b,1,1,f));fail_reads=0;
 assert(!quake_fclose(f));assert(quake_fclose(f)==EOF);
 FILE *all[12];for(int i=0;i<12;i++)assert((all[i]=quake_fopen("/sd/id1/pak0.pak","r")));assert(!quake_fopen("/sd/id1/pak0.pak","r"));
 quake_io_close_all();assert(closes==13);for(int i=0;i<16;i++)assert(!slots[i].open);
 f=quake_fopen("/assets/quake/id1/pak0.pak","rb");assert(f);assert(!quake_fclose(f));
 assert(!quake_fopen("/assets/base.img","rb"));assert(!quake_fopen("/assets/quakex/id1/pak0.pak","rb"));
 quake_snprintf(b,sizeof b,"%g %d",1.25,7);assert(!strcmp(b,"1.25 7"));assert(quake_atof(" -1.25e2")==-125);
 void *p=quake_calloc(4,8);for(int i=0;i<32;i++)assert(!((char *)p)[i]);p=quake_realloc(p,64);quake_free(p);
 puts("Quake adapter: read-only paths, seek, EOF/errors, handles, float formatting and allocation PASS");
}
