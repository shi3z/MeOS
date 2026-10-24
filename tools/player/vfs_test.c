#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <m5os/vfs.h>
#include <m5os/app.h>
#include <m5os/arch.h>
#include <m5os/errno.h>
#include "../../user/filer/name_text.h"
static unsigned depth;static char received[VFS_PATH_MAX];
uint32_t arch_irq_disable(void){return depth++;}
void arch_irq_restore(uint32_t old){assert(depth==old+1);depth=old;}
task_t *task_current(void){return NULL;}
void kprintf(const char *fmt,...){(void)fmt;}
static int open_file(void *ctx,const char *path,uint32_t flags,void **f){(void)ctx;assert(flags==VFS_O_READ);strcpy(received,path);*f=(void *)1;return ERR_OK;}
static int close_file(void *f){assert(f==(void *)1);return ERR_OK;}
const vfs_fs_t vfs_devfs={.name="dev"};
static const vfs_fs_t testfs={.name="test",.open=open_file,.close=close_file};
int main(void){
    char mount[VFS_PATH_MAX],path[VFS_PATH_MAX],out[VFS_PATH_MAX];
    mount[0]='/';memset(mount+1,'a',300);mount[301]=0;
    assert(vfs_mount(mount,&testfs,NULL)==ERR_OK);
    strcpy(path,mount);strcat(path,"/../");strcat(path,mount+1);strcat(path,"/長い名前のファイル.mp3");
    /* This spelling contains two 300-byte components but normalizes to one. */
    assert(vfs_normalize(path,out)==ERR_OK);
    int fd=vfs_open(path,VFS_O_READ);assert(fd>=0 && !strcmp(received,"/長い名前のファイル.mp3"));
    assert(vfs_umount(mount)==ERR_BUSY);assert(vfs_close(fd)==ERR_OK&&vfs_umount(mount)==ERR_OK);
    char excessive[VFS_PATH_MAX+2];excessive[0]='/';memset(excessive+1,'z',VFS_PATH_MAX);excessive[VFS_PATH_MAX+1]=0;
    assert(vfs_normalize(excessive,out)==ERR_NAMETOOLONG);
    char label[96];filer_name_label("録音 🎵 long file name.mp3",label,12);
    assert(strlen(label)<sizeof label && !memchr(label,0xc0,strlen(label)));
    filer_name_label("\xf0",label,23);assert(label[0]=='?');
    assert(!depth);puts("VFS: >255-byte mount matching, UTF-8 paths, overflow rejection and bounded labels PASS");
}
