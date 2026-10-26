/* Every mutation targets a malloc-backed fixture. No physical device is opened. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m5os/fat.h>
#include <m5os/errno.h>
#include <m5os/mutex.h>
#include <m5os/app.h>
static uint8_t *image,*original;
static size_t size;
static const vfs_fs_t *fs;
static int depth,writes,fail_after=-1;
static bool read_perm=true,write_perm=true,tear;
void mutex_lock(mutex_t *m){(void)m;assert(!depth);depth++;}
void mutex_unlock(mutex_t *m){(void)m;assert(depth==1);depth--;}
void *kmalloc(size_t n){return malloc(n);}
void kfree(void *p){free(p);}
bool task_has_perm(uint32_t p){return p==APP_PERM_FS_READ?read_perm:write_perm;}
int vfs_mount(const char *p,const vfs_fs_t *v,void *d){(void)d;assert(!strcmp(p,"/sd")&&!fs);fs=v;return 0;}
int vfs_umount(const char *p){assert(!strcmp(p,"/sd")&&fs);fs=NULL;return 0;}
static int read_blocks(blockdev_t *bd,uint32_t lba,uint32_t n,void *out){
    assert(depth && n && lba<bd->block_count && n<=bd->block_count-lba);
    memcpy(out,image+(size_t)lba*512,(size_t)n*512);return 0;
}
static int write_blocks(blockdev_t *bd,uint32_t lba,uint32_t n,const void *in){
    assert(depth && n && lba<bd->block_count && n<=bd->block_count-lba);
    writes++;if(fail_after==0){if(tear)memcpy(image+(size_t)lba*512,in,173);return ERR_IO;}if(fail_after>0)fail_after--;
    memcpy(image+(size_t)lba*512,in,(size_t)n*512);return 0;
}
static void expect(const char *path,const void *text,size_t len){
    void *f=NULL;assert(fs->open(NULL,path,VFS_O_READ,&f)==0);
    char buf[2048];assert(len<sizeof buf);assert(fs->read(f,buf,sizeof buf)==(int)len && !memcmp(buf,text,len));
    assert(fs->close(f)==0);
}
int main(int argc,char **argv){
    assert(argc==2);FILE *input=fopen(argv[1],"rb");assert(input);
    fseek(input,0,SEEK_END);size=(size_t)ftell(input);rewind(input);
    image=malloc(size);original=malloc(size);assert(image&&original);
    assert(fread(image,1,size,input)==size);fclose(input);memcpy(original,image,size);
    blockdev_t bd={.block_size=512,.block_count=(uint32_t)(size/512),.read=read_blocks,.write=write_blocks};
    assert(!fat_mount(&bd,false) && !fat_is_readonly() && writes==0);
    void *f=NULL,*other=NULL;vfs_info_t info;
    const char *dir="/新しいフォルダー",*path="/新しいフォルダー/文章と𠮷の長い名前.txt";
    write_perm=false;
    assert(fs->mkdir(NULL,dir)==ERR_PERM);
    assert(fs->open(NULL,path,VFS_O_WRITE|VFS_O_CREATE,&f)==ERR_PERM);
    assert(writes==0);write_perm=true;
    assert(!fs->mkdir(NULL,dir));assert(fs->mkdir(NULL,dir)==ERR_EXIST);
    assert(fs->open(NULL,path,VFS_O_READ|VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC,&f)==0);
    assert(fs->open(NULL,path,VFS_O_WRITE,&other)==ERR_BUSY);
    assert(fs->unlink(NULL,path)==ERR_BUSY);
    static const char text[]="日本語と English。SDへ保存する。\n";
    assert(fs->write(f,text,sizeof text-1)==sizeof text-1);
    assert(fs->seek(f,0,FS_SEEK_SET)==0);
    char out[100];assert(fs->read(f,out,sizeof out)==sizeof text-1 && !memcmp(out,text,sizeof text-1));
    assert(!fs->close(f));assert(!fat_unmount());
    assert(!fat_mount(&bd,false));expect(path,text,sizeof text-1);
    assert(fs->unlink(NULL,dir)==ERR_PERM); /* nonempty directory */
    assert(fs->open(NULL,path,VFS_O_WRITE|VFS_O_APPEND,&f)==0);
    assert(fs->seek(f,0,FS_SEEK_SET)==0);assert(fs->write(f,"追記",6)==6);assert(!fs->close(f));
    char combined[100];memcpy(combined,text,sizeof text-1);memcpy(combined+sizeof text-1,"追記",6);
    expect(path,combined,sizeof text-1+6);
    assert(fs->open(NULL,path,VFS_O_WRITE|VFS_O_TRUNC,&f)==0);assert(!fs->close(f));expect(path,"",0);
    assert(fs->open(NULL,"/missing.txt",VFS_O_WRITE|VFS_O_TRUNC,&f)==ERR_NOENT);
    assert(fs->open(NULL,path,VFS_O_CREATE,&f)==ERR_INVAL);
    assert(!fs->unlink(NULL,path));assert(!fs->unlink(NULL,dir));
    assert(fs->stat(NULL,path,&info)==ERR_NOENT);
    assert(!fat_unmount());assert(!fat_mount_readonly(&bd));int before=writes;
    assert(fs->open(NULL,"/readonly.txt",VFS_O_WRITE|VFS_O_CREATE,&f)==ERR_PERM);
    assert(fs->mkdir(NULL,"/no")==ERR_PERM && writes==before);assert(!fat_unmount());
    /* A failed media write must never be reported as saved, and retries must not
     * perform more I/O until explicit unmount/remount. */
    memcpy(image,original,size);assert(!fat_mount(&bd,false));
    assert(!fs->open(NULL,"/HELLO.TXT",VFS_O_WRITE,&f));
    fail_after=0;assert(fs->write(f,text,sizeof text-1)<0);before=writes;
    assert(fs->write(f,text,sizeof text-1)<0 && writes==before);
    assert(fs->close(f)<0);assert(!fat_unmount());
    fail_after=-1;memcpy(image,original,size);assert(!fat_mount(&bd,false));
    assert(!fs->open(NULL,"/HELLO.TXT",VFS_O_WRITE,&f));
    assert(!fs->seek(f,0,FS_SEEK_SET));
    /* Existing data is in cache: fail while synchronizing its sector. */
    fail_after=0;assert(fs->write(f,"changed",7)<0);assert(fs->close(f)<0);assert(!fat_unmount());
    fail_after=-1;
    /* Failure boundaries include partial sector writes. No atomicity claim. */
    if(strstr(argv[1],"fat12")) {
        unsigned char payload[700];for(unsigned j=0;j<sizeof payload;j++)payload[j]=(unsigned char)j;
        memcpy(image,original,size);assert(!fat_mount(&bd,false));int initial=writes;
        assert(!fs->open(NULL,"/NEW.TXT",VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC,&f));
        assert(fs->write(f,payload,sizeof payload)==sizeof payload);assert(!fs->close(f));
        assert(!fat_unmount());int points=writes-initial;assert(points>0);
        for(int partial=0;partial<2;partial++)for(int cut=0;cut<points;cut++) {
            memcpy(image,original,size);fail_after=-1;assert(!fat_mount(&bd,false));
            fail_after=cut;tear=partial!=0;
            int rc=fs->open(NULL,"/NEW.TXT",VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC,&f);
            bool saved=false;
            if(!rc){int n=fs->write(f,payload,sizeof payload);int closed=fs->close(f);saved=n==sizeof payload && closed==0;}
            assert(!saved);assert(!fat_unmount());
        }
        tear=false;fail_after=-1;
    }
    /* Exhaust a small FAT12 fixture and check short writes/ENOSPC, not success. */
    if(strstr(argv[1],"fat12")) {
        memcpy(image,original,size);assert(!fat_mount(&bd,false));
        assert(!fs->open(NULL,"/FULL.BIN",VFS_O_WRITE|VFS_O_CREATE,&f));
        char data[4096];memset(data,0x55,sizeof data);bool full=false;
        for(size_t i=0;i<=size/sizeof data+1;i++) {
            int n=fs->write(f,data,sizeof data);
            if(n<(int)sizeof data){assert(n==ERR_NOSPC || n>=0);full=true;break;}
        }
        assert(full);assert(!fs->close(f));assert(!fat_unmount());
    }
    assert(!depth);free(original);free(image);
    printf("FAT read/write: Japanese create/append/truncate/delete, remount, locks, permissions, disk-full and I/O failures PASS: %s\n",argv[1]);
}
