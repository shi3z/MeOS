#include <m5os/board_config.h>
#if defined(M5OS_BOARD_TAB5)
#include <m5os/sdroot.h>
#include <m5os/vfs.h>
#include <m5os/mutex.h>
#ifndef LFS_THREADSAFE
#define LFS_THREADSAFE
#endif
#include <m5os/sdcard.h>
#include <m5os/fat.h>
#include <m5os/hal_gpio.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/errno.h>
#include <m5os/crc32.h>
#include "../../third_party/littlefs/lfs.h"
#include "../cow/store.h"
#include "../cow/blockdev.h"
#include <string.h>
#define ROOT_SECTORS 16384u /* 8 MiB logical M5FS, sparse zero-filled RO base */
#define BANK_BYTES (12u*1024u*1024u) /* full 8 MiB snapshot + transaction headroom */
#define ERASE_BYTES 4096u
#define CACHE_BYTES 4096u
static mutex_t lfs_mutex=MUTEX_INIT;
static int lock_lfs(const struct lfs_config *c){(void)c;mutex_lock(&lfs_mutex);return 0;}
static int unlock_lfs(const struct lfs_config *c){(void)c;mutex_unlock(&lfs_mutex);return 0;}
static blockdev_t *sd;
static lfs_t lfs;
static struct lfs_config cfg;
static unsigned char lookahead[128];
struct root_buffers {
    unsigned char read_cache[CACHE_BYTES],prog_cache[CACHE_BYTES];
    unsigned char bounce[ERASE_BYTES],base_cache[CACHE_BYTES];
    unsigned char bank_cache[2][CACHE_BYTES];
};
static struct root_buffers *buffers;
static lfs_file_t base_file;
static struct lfs_file_config base_cfg;
struct bank_file { lfs_file_t file; struct lfs_file_config cfg; };
static struct bank_file files[2];
static struct cow_store store;
static blockdev_t root_bd;
static uint32_t *index_map;
static uint64_t identity;
static bool mounted,active,opened;
static void mount_mark(const char *stage,uint64_t start,uint64_t *last) {
    uint64_t now=hal_time_now_us();
    kprintf("SDROOT +%u ms (%u ms) %s\n",(uint32_t)((now-start)/1000u),(uint32_t)((now-*last)/1000u),stage);
    *last=now;
}
static int disk_read(const struct lfs_config *c,lfs_block_t b,lfs_off_t off,void *p,lfs_size_t n) {
    (void)c;return sd->read(sd,b*8+off/512,n/512,p)?LFS_ERR_IO:0;
}
static int disk_prog(const struct lfs_config *c,lfs_block_t b,lfs_off_t off,const void *p,lfs_size_t n) {
    (void)c;return sd->write(sd,b*8+off/512,n/512,p)?LFS_ERR_IO:0;
}
static int disk_erase(const struct lfs_config *c,lfs_block_t b) {
    (void)c;memset(buffers->bounce,0xff,sizeof buffers->bounce);
    return sd->write(sd,b*8,ERASE_BYTES/512,buffers->bounce)?LFS_ERR_IO:0;
}
static int disk_sync(const struct lfs_config *c) {
    (void)c; /* CMD24 waits for ready and checks CMD13 after every sector. */
    sdcard_status_t st;sdcard_status(&st);return st.ready?0:LFS_ERR_IO;
}
static int prepare(void) {
    sd=NULL;
    for(unsigned attempt=0;attempt<3;attempt++) {
        sd=sdcard_probe();if(sd)break;
        sdcard_status_t st;sdcard_status(&st);
        kprintf("sdroot: probe %u failed: %s (R1=0x%02x)\n",attempt+1,st.detail,st.last_r1);
        if(attempt<2)hal_delay_us(100000);
    }
    if(!sd)return ERR_NODEV;
    if(!buffers){buffers=kmalloc_psram(sizeof *buffers);if(!buffers)return ERR_NOMEM;}
    cfg=(struct lfs_config){.lock=lock_lfs,.unlock=unlock_lfs,.read=disk_read,.prog=disk_prog,.erase=disk_erase,.sync=disk_sync,
        .read_size=512,.prog_size=512,.block_size=ERASE_BYTES,.block_count=sd->block_count/8,
        .block_cycles=500,.cache_size=CACHE_BYTES,.lookahead_size=sizeof lookahead,
        .read_buffer=buffers->read_cache,.prog_buffer=buffers->prog_cache,.lookahead_buffer=lookahead};
    return cfg.block_count>=128?0:ERR_INVAL;
}
/* Bytes beyond EOF represent zero sectors, not an image-sized RAM buffer.
 * Only the formatter writes base.img; runtime opens it read-only. */
static int file_read(lfs_file_t *f,uint32_t o,void *p,size_t n) {
    lfs_soff_t size=lfs_file_size(&lfs,f);if(size<0)return -1;
    memset(p,0,n);if(o>=(uint32_t)size)return 0;
    size_t take=n;if(take>(uint32_t)size-o)take=(uint32_t)size-o;
    if(lfs_file_seek(&lfs,f,o,LFS_SEEK_SET)!=(lfs_soff_t)o)return -1;
    return lfs_file_read(&lfs,f,p,take)==(lfs_ssize_t)take?0:-1;
}
static int file_write(lfs_file_t *f,uint32_t o,const void *p,size_t n) {
    if(lfs_file_seek(&lfs,f,o,LFS_SEEK_SET)!=(lfs_soff_t)o)return -1;
    return lfs_file_write(&lfs,f,p,n)==(lfs_ssize_t)n?0:-1;
}
static int base_read(void *ctx,uint32_t lba,void *p){(void)ctx;return file_read(&base_file,lba*512,p,512);}
static int base_rd(blockdev_t *bd,uint32_t l,uint32_t n,void *p){(void)bd;return file_read(&base_file,l*512,p,n*512)?ERR_IO:0;}
static int base_wr(blockdev_t *bd,uint32_t l,uint32_t n,const void *p){(void)bd;return file_write(&base_file,l*512,p,n*512)?ERR_IO:0;}
static int bank_read(void *ctx,uint32_t o,void *p,size_t n){return file_read(&((struct bank_file *)ctx)->file,o,p,n);}
static int bank_write(void *ctx,uint32_t o,const void *p,size_t n){return file_write(&((struct bank_file *)ctx)->file,o,p,n);}
static int bank_sync(void *ctx){return lfs_file_sync(&lfs,&((struct bank_file *)ctx)->file);}
static int bank_reset(void *ctx){return lfs_file_truncate(&lfs,&((struct bank_file *)ctx)->file,0);}
static struct cow_bank banks[2];
static void close_files(void) {
    if(opened){for(int i=0;i<2;i++)lfs_file_close(&lfs,&files[i].file);lfs_file_close(&lfs,&base_file);opened=false;}
    if(mounted){lfs_unmount(&lfs);mounted=false;}
    kfree(index_map);index_map=NULL;active=false;
}
static int open_base(int flags) {
    memset(&base_cfg,0,sizeof base_cfg);base_cfg.buffer=buffers->base_cache;
    return lfs_file_opencfg(&lfs,&base_file,"base.img",flags,&base_cfg);
}
static int base_identity(void) {
    lfs_soff_t size=lfs_file_size(&lfs,&base_file);if(size<=0||size>64*1024)return -1;
    uint32_t sum=0;
    for(uint32_t off=0;off<(uint32_t)size;off+=512){
        if(file_read(&base_file,off,buffers->bounce,512))return -1;
        sum=crc32_update(sum,buffers->bounce,512);
    }
    identity=((uint64_t)sum<<32)|ROOT_SECTORS;return 0;
}
static int open_banks(bool create) {
    for(int i=0;i<2;i++){
        memset(&files[i],0,sizeof files[i]);files[i].cfg.buffer=buffers->bank_cache[i];
        if(lfs_file_opencfg(&lfs,&files[i].file,i?"delta-b.log":"delta-a.log",LFS_O_RDWR|(create?LFS_O_CREAT:0),&files[i].cfg)){
            while(i-->0)lfs_file_close(&lfs,&files[i].file);
            lfs_file_close(&lfs,&base_file);return -1;
        }
        banks[i]=(struct cow_bank){{&files[i],BANK_BYTES,bank_read,bank_write,bank_sync},bank_reset};
    }
    opened=true;return 0;
}
static int attach_index(void){return cow_index(&store.view,index_map,ROOT_SECTORS);}
static int begin(void *ctx){(void)ctx;if(!store.view.index&&attach_index())return -1;return cow_store_begin(&store);}
static int reserve(void *ctx,uint32_t records){(void)ctx;return cow_store_reserve(&store,records);}
static int commit(void *ctx){(void)ctx;return cow_store_commit(&store);}
static int recover(void *ctx){
    (void)ctx;
    if(cow_store_recover_indexed(&store,index_map,ROOT_SECTORS))return -1;
    return store.view.sealed?cow_store_compact(&store):0;
}
int sdroot_mount(void) {
    /* Refuse competing owners before prepare can reset/probe the SD bus. */
    if(active||mounted||fat_is_mounted())return ERR_BUSY;
    uint64_t start=hal_time_now_us(),last=start;
    int rc=prepare();if(rc)return rc;
    mount_mark("card-ready",start,&last);
    if(lfs_mount(&lfs,&cfg))return ERR_CORRUPT;
    mount_mark("littlefs-mounted",start,&last);
    mounted=true;
    if(open_base(LFS_O_RDONLY))goto fail;
    if(base_identity()){lfs_file_close(&lfs,&base_file);goto fail;}
    mount_mark("base-identified",start,&last);
    index_map=kmalloc_psram(ROOT_SECTORS*sizeof *index_map);if(!index_map)goto fail;
    if(open_banks(false))goto fail;
    if(cow_store_open_indexed(&store,banks,identity,ROOT_SECTORS,base_read,NULL,index_map,ROOT_SECTORS))goto fail;
    if(cow_store_reserve(&store,32))goto fail;
    mount_mark("cow-recovered+indexed",start,&last);
    cow_blockdev_init(&root_bd,&store.view);
    fs_tx_backend_t backend={.begin=begin,.commit=commit,.recover=recover,.reserve=reserve};
    if(fs_mount_atomic(&root_bd,0,&backend))goto fail;
    mount_mark("m5fs-mounted",start,&last);
    active=true;return 0;
fail:close_files();return ERR_IO;
}
int sdroot_format(uint32_t expected_sectors) {
    if(active||mounted||fat_is_mounted())return ERR_BUSY;
    int rc=prepare();if(rc)return rc;
    if(expected_sectors!=sd->block_count)return ERR_INVAL;
    kprintf("SDROOT FORMAT identified sectors=%u; replacing filesystem with littlefs\n",sd->block_count);
    if(lfs_format(&lfs,&cfg)||lfs_mount(&lfs,&cfg))return ERR_IO;
    mounted=true;
    if(open_base(LFS_O_RDWR|LFS_O_CREAT|LFS_O_TRUNC))goto fail;
    blockdev_t base={.name="base",.block_size=512,.block_count=ROOT_SECTORS,.read=base_rd,.write=base_wr};
    rc=fs_format(&base,0,ROOT_SECTORS);if(rc||lfs_file_sync(&lfs,&base_file)){
        lfs_file_close(&lfs,&base_file);goto fail;
    }
    if(base_identity()){lfs_file_close(&lfs,&base_file);goto fail;}
    if(open_banks(true))goto fail;
    if(cow_store_format(&store,banks,identity,ROOT_SECTORS,base_read,NULL))goto fail;
    close_files();kputs("SDROOT FORMAT PASS: littlefs + immutable sparse base + two delta banks; reboot to use\n");return 0;
fail:close_files();return ERR_IO;
}
bool sdroot_active(void){return active;}
void sdroot_status(void){
    sdcard_status_t st;sdcard_status(&st);
    kprintf("SDROOT active=%u card_sectors=%u high_capacity=%u littlefs=%u\n",active,st.sectors,st.high_capacity,mounted);
    if(active)kprintf("SDROOT epoch=%u bank=%u committed=%u logical_KiB=%u index_KiB=64\n",(uint32_t)store.epoch,store.selected,store.view.committed,ROOT_SECTORS/2);
}
static int save(const fs_tx_ops_t *ops,void *ctx){
    const char *text=ctx;int fd=ops->open("/home/sd-boot.txt",FS_O_WRITE|FS_O_CREATE|FS_O_TRUNC);
    if(fd<0)return fd;
    size_t n=strlen(text);int rc=ops->write(fd,text,n);if(rc!=(int)n)return rc<0?rc:ERR_IO;
    return ops->close(fd);
}
int sdroot_save_probe(const char *text){return active?fs_transaction(save,(void *)text):ERR_NODEV;}
/* Separate bulk files share littlefs, never the base image or COW bank paths. */
struct asset_file { lfs_file_t file; struct lfs_file_config cfg; unsigned char cache[]; };
static int asset_error(int n){
    if(n>=0)return n;
    switch(n){case LFS_ERR_NOENT:return ERR_NOENT;case LFS_ERR_EXIST:return ERR_EXIST;
    case LFS_ERR_NOSPC:return ERR_NOSPC;case LFS_ERR_NOMEM:return ERR_NOMEM;
    case LFS_ERR_NOTDIR:return ERR_NOTDIR;case LFS_ERR_ISDIR:return ERR_ISDIR;
    case LFS_ERR_NOTEMPTY:return ERR_NOTEMPTY;case LFS_ERR_INVAL:return ERR_INVAL;
    default:return ERR_IO;}
}
static int asset_path(const char *rel,char out[256]){
    /* VFS supplies normalized absolute relative paths. Reject traversal also at
     * this boundary, so internal root-store files cannot ever be addressed. */
    if(!active)return ERR_NODEV;
    if(!rel||rel[0]!='/'||strlen(rel)>240)return ERR_INVAL;
    const char *p=rel;
    while(*p){while(*p=='/')p++;const char *start=p;while(*p&&*p!='/')p++;
        size_t n=(size_t)(p-start);if((n==1&&start[0]=='.')||(n==2&&start[0]=='.'&&start[1]=='.'))return ERR_INVAL;
    }
    strcpy(out,"/assets");strcat(out,rel);return 0;
}
static int asset_open(void *ctx,const char *rel,uint32_t flags,void **out){
    (void)ctx;char path[256];int rc=asset_path(rel,path);if(rc)return rc;
    if(!(flags&(VFS_O_READ|VFS_O_WRITE)))return ERR_INVAL;
    int mode=(flags&VFS_O_WRITE)?((flags&VFS_O_READ)?LFS_O_RDWR:LFS_O_WRONLY):LFS_O_RDONLY;
    if(flags&VFS_O_CREATE)mode|=LFS_O_CREAT;
    if(flags&VFS_O_TRUNC)mode|=LFS_O_TRUNC;
    if(flags&VFS_O_APPEND)mode|=LFS_O_APPEND;
    struct asset_file *f=kmalloc_psram(sizeof *f+CACHE_BYTES);if(!f)return ERR_NOMEM;
    memset(f,0,sizeof *f+CACHE_BYTES);f->cfg.buffer=f->cache;
    rc=lfs_file_opencfg(&lfs,&f->file,path,mode,&f->cfg);
    if(rc){kfree(f);return asset_error(rc);}*out=f;return 0;
}
static int asset_close(void *file){struct asset_file *f=file;int rc=lfs_file_close(&lfs,&f->file);kfree(f);return asset_error(rc);}
static int asset_read(void *file,void *buf,size_t n){return asset_error(lfs_file_read(&lfs,&((struct asset_file *)file)->file,buf,n));}
static int asset_write(void *file,const void *buf,size_t n){return asset_error(lfs_file_write(&lfs,&((struct asset_file *)file)->file,buf,n));}
static int asset_seek(void *file,int32_t offset,int whence){return asset_error(lfs_file_seek(&lfs,&((struct asset_file *)file)->file,offset,whence));}
static int asset_stat(void *ctx,const char *rel,vfs_info_t *out){
    (void)ctx;char path[256];int rc=asset_path(rel,path);if(rc)return rc;
    struct lfs_info info;rc=lfs_stat(&lfs,path,&info);if(rc)return asset_error(rc);
    memset(out,0,sizeof *out);strcpy(out->name,info.name);out->size=info.size;
    out->type=info.type==LFS_TYPE_DIR?FS_TYPE_DIR:FS_TYPE_FILE;return 0;
}
static int asset_mkdir(void *ctx,const char *rel){(void)ctx;char path[256];int rc=asset_path(rel,path);return rc?rc:asset_error(lfs_mkdir(&lfs,path));}
static int asset_unlink(void *ctx,const char *rel){(void)ctx;char path[256];int rc=asset_path(rel,path);if(rc)return rc;if(!strcmp(path,"/assets/"))return ERR_PERM;return asset_error(lfs_remove(&lfs,path));}
static int asset_readdir(void *ctx,const char *rel,int (*cb)(const vfs_info_t *,void *),void *arg){
    (void)ctx;char path[256];int rc=asset_path(rel,path);if(rc)return rc;
    lfs_dir_t dir;rc=lfs_dir_open(&lfs,&dir,path);if(rc)return asset_error(rc);
    struct lfs_info info;int count=0;
    while((rc=lfs_dir_read(&lfs,&dir,&info))>0){
        if(!strcmp(info.name,".")||!strcmp(info.name,".."))continue;
        vfs_info_t entry={0};strcpy(entry.name,info.name);entry.size=info.size;
        entry.type=info.type==LFS_TYPE_DIR?FS_TYPE_DIR:FS_TYPE_FILE;count++;
        if(cb(&entry,arg))break;
    }
    int close=lfs_dir_close(&lfs,&dir);return rc<0?asset_error(rc):close<0?asset_error(close):count;
}
static const vfs_fs_t assets_vfs={.name="sd-assets",.open=asset_open,.close=asset_close,
    .read=asset_read,.write=asset_write,.seek=asset_seek,.stat=asset_stat,
    .mkdir=asset_mkdir,.unlink=asset_unlink,.readdir=asset_readdir};
int sdroot_assets_mount(void){
    if(!active)return ERR_NODEV;
    int rc=lfs_mkdir(&lfs,"/assets");if(rc&&rc!=LFS_ERR_EXIST)return asset_error(rc);
    return vfs_mount("/assets",&assets_vfs,NULL);
}
#endif
