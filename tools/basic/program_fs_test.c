#include <m5os/program_fs.h>
#include <m5os/errno.h>
#include <m5os/mutex.h>
#include <m5os/app.h>
#include "../../user/basic/file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t flash[PROGRAM_STORE_BYTES];static int depth,fail_write,alloc_fail,read_perm=1,write_perm=1;static unsigned allocations,erases;
static const vfs_fs_t *mounted_fs;static void *handles[8];
void mutex_lock(mutex_t *m) {(void)m;assert(!depth);depth=1;}
void mutex_unlock(mutex_t *m) {(void)m;assert(depth==1);depth=0;}
bool task_has_perm(uint32_t p) {return (!(p&APP_PERM_FS_READ)||read_perm)&&(!(p&APP_PERM_FS_WRITE)||write_perm);}
void *kmalloc_psram(size_t n) {if(alloc_fail)return NULL;void *p=malloc(n);if(p)allocations++;return p;}
void *kmalloc(size_t n) {return kmalloc_psram(n);}
void kfree(void *p) {if(p){assert(allocations);allocations--;free(p);}}
static int rd(void *ctx,uint32_t off,void *buf,size_t n) {(void)ctx;assert(depth&&off<=sizeof flash&&n<=sizeof flash-off);if(n)memcpy(buf,flash+off,n);return 0;}
static int wr(void *ctx,uint32_t off,const void *data,size_t n) {(void)ctx;assert(depth&&off<=sizeof flash&&n<=sizeof flash-off);if(fail_write)return ERR_IO;for(size_t i=0;i<n;i++){assert((flash[off+i]&((const uint8_t *)data)[i])==((const uint8_t *)data)[i]);flash[off+i]&=((const uint8_t *)data)[i];}return 0;}
static int erase(void *ctx,uint32_t off) {(void)ctx;assert(depth&&!(off&4095)&&off<=sizeof flash-4096);memset(flash+off,255,4096);erases++;return 0;}
static program_store_io io={.read=rd,.write=wr,.erase=erase};
int vfs_mount(const char *p,const vfs_fs_t *fs,void *data) {(void)data;assert(!strcmp(p,"/programs")&&!mounted_fs);mounted_fs=fs;return 0;}
int vfs_umount(const char *p) {assert(!strcmp(p,"/programs")&&mounted_fs);mounted_fs=NULL;return 0;}
static const char *relative(const char *p) {assert(!strncmp(p,"/programs",9));return p[9]?p+9:"/";}
int vfs_open(const char *p,uint32_t flags) {
    if(!mounted_fs)return ERR_NOENT;
    unsigned i;
    for(i=0;i<8&&handles[i];i++) {}
    if(i==8)return ERR_BUSY;
    int rc=mounted_fs->open(NULL,relative(p),flags,&handles[i]);return rc<0?rc:(int)i;
}
int vfs_close(int fd) {assert(fd>=0&&fd<8&&handles[fd]);int rc=mounted_fs->close(handles[fd]);handles[fd]=NULL;return rc;}
int vfs_read(int fd,void *out,size_t n) {assert(handles[fd]);return mounted_fs->read(handles[fd],out,n);}
int vfs_write(int fd,const void *data,size_t n) {assert(handles[fd]);return mounted_fs->write(handles[fd],data,n);}
int vfs_stat(const char *p,vfs_info_t *out) {return mounted_fs?mounted_fs->stat(NULL,relative(p),out):ERR_NOENT;}
static void output(const char *p,size_t n,void *ctx) {(void)p;(void)n;(void)ctx;}
static int listed(const vfs_info_t *info,void *ctx) {
    unsigned *n=ctx;assert(!depth);(*n)++;assert(info->type==FS_TYPE_FILE);
    vfs_info_t stat;char path[VFS_PATH_MAX];snprintf(path,sizeof path,"/programs/%s",info->name);assert(!vfs_stat(path,&stat));
    assert(program_fs_unmount()==ERR_BUSY);return 0;
}
int main(void) {
    memset(flash,255,sizeof flash);assert(program_fs_mount(&io)==ERR_NOENT&&!erases);
    assert(!program_fs_init(&io)&&!erases);assert(!program_fs_mount(&io)&&program_fs_ready());
    basic_session *s=malloc(sizeof *s);assert(s);basic_session_init(s,output,NULL,NULL);
    basic_session_submit(s,"10 PRINT .125*3");basic_file_result result;
    assert(!basic_file_save(s,"/programs/demo.bas",&result));assert(erases==5);
    assert(!program_fs_unmount());assert(!program_fs_mount(&io));
    basic_session_submit(s,"NEW");assert(!basic_file_load(s,"/programs/demo.bas",&result));assert(!basic_session_run(s));
    size_t source_len=basic_session_export(s);assert(!strcmp(s->program,"10 PRINT .125*3\n"));
    int r=vfs_open("/programs/demo.bas",VFS_O_READ);assert(r>=0);
    int w=vfs_open("/programs/demo.bas",VFS_O_WRITE|VFS_O_TRUNC);assert(w>=0);
    assert(vfs_open("/programs/other.bas",VFS_O_WRITE|VFS_O_CREATE)==ERR_BUSY);
    assert(program_fs_unmount()==ERR_BUSY);assert(vfs_write(w,"10 PRINT 7\n",11)==11);assert(!vfs_close(w));
    char old[64];assert(vfs_read(r,old,sizeof old)==(int)source_len&&!memcmp(old,"10 PRINT .125*3\n",source_len));assert(!vfs_close(r));
    assert(!basic_file_load(s,"/programs/demo.bas",&result));assert(!strcmp(s->source.lines[0].text,"PRINT 7"));
    /* Failed close leaves old committed source, blocks further writes, and can remount. */
    basic_session_submit(s,"10 PRINT 9");fail_write=1;assert(basic_file_save(s,"/programs/demo.bas",&result)==ERR_IO);
    assert(!program_fs_ready()&&program_fs_mounted());fail_write=0;assert(!program_fs_unmount());assert(!program_fs_mount(&io));
    assert(!basic_file_load(s,"/programs/demo.bas",&result));assert(!strcmp(s->source.lines[0].text,"PRINT 7"));
    /* A rejected write after TRUNC must abort close, not commit an empty file. */
    unsigned erase_before=erases;
    w=vfs_open("/programs/demo.bas",VFS_O_WRITE|VFS_O_TRUNC);assert(w>=0);
    assert(vfs_write(w,"x",PROGRAM_STORE_MAX+1)==ERR_NOSPC);assert(vfs_close(w)==ERR_NOSPC&&erases==erase_before);
    assert(!basic_file_load(s,"/programs/demo.bas",&result)&&!strcmp(s->source.lines[0].text,"PRINT 7"));
    w=vfs_open("/programs/demo.bas",VFS_O_WRITE|VFS_O_TRUNC);assert(w>=0);write_perm=0;
    assert(vfs_write(w,"x",1)==ERR_PERM);write_perm=1;assert(vfs_close(w)==ERR_PERM&&erases==erase_before);
    /* Read/write/append/seek semantics, including a zero-filled gap. */
    w=vfs_open("/programs/bytes",VFS_O_READ|VFS_O_WRITE|VFS_O_CREATE);assert(w>=0);
    assert(vfs_write(w,"abc",3)==3);assert(mounted_fs->seek(handles[w],5,FS_SEEK_SET)==5);assert(!vfs_read(w,old,sizeof old));
    assert(vfs_write(w,"Z",1)==1);assert(!vfs_close(w));
    w=vfs_open("/programs/bytes",VFS_O_WRITE|VFS_O_APPEND);assert(w>=0);assert(vfs_write(w,"!",1)==1);assert(!vfs_close(w));
    r=vfs_open("/programs/bytes",VFS_O_READ);assert(r>=0);assert(vfs_read(r,old,sizeof old)==7&&!memcmp(old,"abc\0\0Z!",7));
    assert(vfs_write(r,"x",1)==ERR_PERM);assert(!vfs_close(r));
    unsigned n=0;assert(mounted_fs->readdir(NULL,"/",listed,&n)==2&&n==2);
    alloc_fail=1;assert(vfs_open("/programs/demo.bas",VFS_O_READ)==ERR_NOMEM);alloc_fail=0;
    read_perm=0;vfs_info_t info;assert(vfs_stat("/programs/demo.bas",&info)==ERR_PERM);read_perm=1;
    write_perm=0;assert(vfs_open("/programs/x",VFS_O_WRITE|VFS_O_CREATE)==ERR_PERM);assert(program_fs_unmount()==ERR_PERM);write_perm=1;
    assert(vfs_open("/programs/../bad",VFS_O_WRITE|VFS_O_CREATE)==ERR_INVAL);
    assert(!mounted_fs->unlink(NULL,"/bytes"));assert(vfs_stat("/programs/bytes",&info)==ERR_NOENT);
    assert(!program_fs_unmount()&&!allocations);free(s);puts("program VFS / BASIC save-remount-load / failure tests PASS");
}
