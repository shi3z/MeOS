#include <m5os/board_config.h>
#include <m5os/rootfs.h>
#include <m5os/vfs.h>
#include <m5os/m5ex.h>
#include <m5os/kmalloc.h>
#include <m5os/errno.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static blockdev_t ram,legacy;
static int sd_result,legacy_result,format_result,vfs_result;
static bool no_ram;
static unsigned probes,ram_allocations,formats,mounts,root_mounts,seeds,imports;
static size_t psram_bytes;
static uint32_t allocated_blocks;
bool rootfs_mount(blockdev_t *);
int sdroot_mount(void){probes++;return sd_result;}
bool sdroot_active(void){return sd_result==ERR_OK;}
int sdroot_assets_mount(void){return ERR_OK;}
void wasm_store_boot(void){imports++;}
void kputs(const char *s){(void)s;}
void kprintf(const char *fmt,...){(void)fmt;}
int ksnprintf(char *out,size_t n,const char *fmt,...){
    va_list ap;va_start(ap,fmt);int rc=vsnprintf(out,n,fmt,ap);va_end(ap);return rc;
}
void kmem_psram_stats(kmem_stats_t *s){*s=(kmem_stats_t){.total=psram_bytes};}
blockdev_t *ramdisk_create(uint32_t blocks){ram_allocations++;allocated_blocks=blocks;return no_ram?NULL:&ram;}
int fs_format(blockdev_t *bd,uint32_t lba,uint32_t blocks){
    assert(bd==&ram&&lba==0&&blocks==allocated_blocks);formats++;return format_result;
}
int fs_mount(blockdev_t *bd,uint32_t lba){
    assert(!lba&&(bd==&ram||bd==&legacy));mounts++;return bd==&legacy?legacy_result:ERR_OK;
}
const vfs_fs_t vfs_m5fs={.name="test-m5fs"};
int vfs_mount(const char *path,const vfs_fs_t *fs,void *ctx){
    assert(!strcmp(path,"/")&&fs==&vfs_m5fs&&!ctx);root_mounts++;return vfs_result;
}
int vfs_open(const char *path,uint32_t flags){
    assert(!strncmp(path,"/system/",8)||!strncmp(path,"/home/",6)||!strncmp(path,"/apps/",6));
    assert(flags==(VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC));seeds++;return 1;
}
int vfs_write(int fd,const void *p,size_t n){assert(fd==1&&p);return (int)n;}
int vfs_close(int fd){assert(fd==1);return ERR_OK;}
const m5ex_image_t *m5ex_embedded(int *count){*count=0;return NULL;}
static void reset(void){
    probes=ram_allocations=formats=mounts=root_mounts=seeds=imports=0;
    allocated_blocks=0;psram_bytes=32u*1024u*1024u;
    sd_result=ERR_NODEV;legacy_result=ERR_CORRUPT;format_result=vfs_result=ERR_OK;no_ram=false;
}
static void restored_apps(void){
#if M5OS_BOARD_TAB5
    assert(imports==1);
#else
    assert(!imports);
#endif
}
int main(void){
#if M5OS_BOARD_TAB5
    reset();sd_result=ERR_OK;
    assert(rootfs_mount(&legacy)&&rootfs_is_persistent());
    assert(probes==1&&!ram_allocations&&!formats&&!mounts&&!seeds&&root_mounts==1);restored_apps();
#endif
    const int errors[]={ERR_NODEV,ERR_CORRUPT,ERR_IO};
    for(unsigned i=0;i<sizeof errors/sizeof errors[0];i++){
        reset();sd_result=errors[i];assert(rootfs_mount(NULL)&&!rootfs_is_persistent());
        assert(ram_allocations==1&&formats==1&&mounts==1&&root_mounts==1&&seeds==3);
        assert(allocated_blocks==16384);restored_apps();
#if M5OS_BOARD_TAB5
        assert(probes==1);
#else
        assert(!probes);
#endif
    }
    reset();legacy_result=ERR_OK;
    assert(rootfs_mount(&legacy)&&rootfs_is_persistent());
    assert(!ram_allocations&&!formats&&mounts==1&&!seeds);restored_apps();
    reset();assert(rootfs_mount(&legacy)&&!rootfs_is_persistent());
    assert(ram_allocations==1&&formats==1&&mounts==2);restored_apps();
    reset();psram_bytes=8u*1024u*1024u;assert(rootfs_mount(NULL)&&allocated_blocks==4096);
    reset();psram_bytes=0;assert(rootfs_mount(NULL)&&allocated_blocks==128);
    reset();no_ram=true;assert(!rootfs_mount(NULL)&&!formats&&!mounts&&!root_mounts&&!imports);
    reset();format_result=ERR_IO;assert(!rootfs_mount(NULL)&&formats==1&&!mounts&&!root_mounts&&!imports);
    reset();vfs_result=ERR_BUSY;assert(!rootfs_mount(NULL)&&root_mounts==1&&!imports);
    puts("Root boot selection: persistent SD, RAM-only format fallback, legacy root, memory budgets and failures PASS");
}
