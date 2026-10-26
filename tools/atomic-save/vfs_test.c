#include <m5os/vfs.h>
#include <m5os/app.h>
#include <m5os/errno.h>
#include <assert.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
static unsigned depth,opens,closes,writes,replacements;
static int replacement_error,write_error,close_error,abandon;
static size_t position,length;
static char disk[100];
static jmp_buf stopped;
static app_t owner={.app_id=7};
static task_t task={.app=&owner};
uint32_t arch_irq_disable(void){return depth++;}
void arch_irq_restore(uint32_t old){assert(depth==old+1);depth=old;}
task_t *task_current(void){return &task;}
void kprintf(const char *fmt,...){(void)fmt;}
const vfs_fs_t vfs_devfs={.name="test-dev"};
static int open_file(void *ctx,const char *path,uint32_t flags,void **f){
    (void)ctx;assert(!depth);assert(path[0]=='/');opens++;*f=(void *)1;
    if(flags&VFS_O_TRUNC){length=position=0;}
    return 0;
}
static int close_file(void *f){assert(f==(void *)1&&!depth);closes++;return close_error;}
static int write_file(void *f,const void *b,size_t n){
    assert(f==(void *)1&&!depth);writes++;
    if(write_error&&position>=2)return write_error;
    if(n>2)n=2;
    assert(position+n<sizeof disk);memcpy(disk+position,b,n);position+=n;length=position;return (int)n;
}
static int replace(void *ctx,const char *path,const void *data,size_t n,size_t *written){
    (void)ctx;assert(!depth);replacements++;
    assert(!strcmp(path,"/note"));
    assert(vfs_count_owned(7)==1);assert(vfs_umount("/home")==ERR_BUSY);
    char b;assert(vfs_read(0,&b,1)==ERR_BADF); /* transient pin isn't an open file */
    if(abandon)longjmp(stopped,1);
    if(replacement_error)return replacement_error;
    memcpy(disk,data,n);length=n;*written=n;return 0;
}
static const vfs_fs_t stream={.name="stream",.open=open_file,.close=close_file,.write=write_file};
static const vfs_fs_t atomic={.name="atomic",.open=open_file,.close=close_file,.write=write_file,.replace=replace};
int main(void){
    size_t bytes=99;
    assert(vfs_replace_file("relative","x",1,&bytes)==ERR_INVAL&&bytes==0);
    assert(vfs_replace_file("/none",NULL,1,&bytes)==ERR_INVAL&&bytes==0);
    assert(vfs_replace_file("/none","x",1,&bytes)==ERR_NOENT&&bytes==0);
    assert(!vfs_mount("/home",&atomic,NULL));
    assert(!vfs_replace_file("/home/x/../note","new",3,&bytes)&&bytes==3);
    assert(length==3&&!memcmp(disk,"new",3)&&!opens&&!writes&&!closes);
    int errors[]={ERR_PERM,ERR_IO,ERR_NOSYS,ERR_NOSPC};
    for(unsigned i=0;i<sizeof errors/sizeof errors[0];i++){
        replacement_error=errors[i];bytes=99;
        assert(vfs_replace_file("/home/note","bad",3,&bytes)==errors[i]&&bytes==0);
        assert(!opens&&!writes&&!closes&&length==3&&!memcmp(disk,"new",3));
        assert(!vfs_count_owned(7));
    }
    replacement_error=0;abandon=1;
    if(!setjmp(stopped)){(void)vfs_replace_file("/home/note","dead",4,NULL);assert(0);}
    assert(vfs_umount("/home")==ERR_BUSY);
    assert(vfs_close_owned(7)==1);assert(!closes);assert(!vfs_count_owned(7));
    assert(!vfs_umount("/home"));
    assert(!vfs_mount("/",&stream,NULL));
    int fds[VFS_FD_MAX-1];
    for(unsigned i=0;i<VFS_FD_MAX-1;i++){fds[i]=vfs_open("/held",VFS_O_READ);assert(fds[i]>=0);}
    /* Legacy save can use the final free descriptor; no extra pin is consumed. */
    assert(!vfs_replace_file("/legacy","1234567",7,&bytes)&&bytes==7);
    assert(length==7&&!memcmp(disk,"1234567",7));
    for(unsigned i=0;i<VFS_FD_MAX-1;i++)assert(!vfs_close(fds[i]));
    write_error=ERR_IO;close_error=ERR_PERM;
    assert(vfs_replace_file("/legacy","partial",7,&bytes)==ERR_IO&&bytes==2);
    write_error=0;assert(vfs_replace_file("/legacy","closed",6,&bytes)==ERR_PERM&&bytes==6);
    close_error=0;assert(!vfs_replace_file("/legacy",NULL,0,&bytes)&&bytes==0);
    assert(!length&&!vfs_count_owned(7)&&!depth);assert(!vfs_umount("/"));
    puts("VFS save dispatch PASS: mount pin/owner cleanup, no fallback after errors, permissions, legacy short writes/close errors/full descriptor table");
}
