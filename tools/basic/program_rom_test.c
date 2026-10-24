#include <m5os/program_fs.h>
#include <m5os/errno.h>
#include <m5os/mutex.h>
#include <m5os/app.h>
#include "esp_rom_spiflash.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t flash[0x1000000];static unsigned reads,writes,erases,irq,depth;static const vfs_fs_t *fs;
esp_rom_spiflash_chip_t g_rom_flashchip={sizeof flash};
uint32_t arch_irq_disable(void) {assert(!irq);irq=1;return 0;}
void arch_irq_restore(uint32_t flags) {assert(!flags&&irq);irq=0;}
void task_sleep(uint32_t ms) {assert(!irq&&ms==1);}
void mutex_lock(mutex_t *m) {(void)m;assert(!depth);depth=1;}
void mutex_unlock(mutex_t *m) {(void)m;assert(depth==1);depth=0;}
bool task_has_perm(uint32_t p) {(void)p;return true;}
void *kmalloc_psram(size_t n) {return malloc(n);}
void *kmalloc(size_t n) {return malloc(n);}
void kfree(void *p) {free(p);}
void kprintf(const char *s,...) {(void)s;}
void kputs(const char *s) {(void)s;}
int vfs_mount(const char *p,const vfs_fs_t *v,void *ctx) {(void)ctx;assert(!strcmp(p,"/data")&&!fs);fs=v;return 0;}
int vfs_umount(const char *p) {assert(!strcmp(p,"/data")&&fs);fs=NULL;return 0;}
int esp_rom_spiflash_unlock(void) {assert(irq);return 0;}
int esp_rom_spiflash_read(uint32_t off,uint32_t *out,int32_t n) {
    assert(irq&&n>0&&!(off&3)&&!((uintptr_t)out&3)&&!(n&3));
    assert((off>=0x8000&&off+(unsigned)n<=0x9000)||(off>=0xc00000&&off+(unsigned)n<=0xd00000));
    memcpy(out,flash+off,(size_t)n);reads++;return 0;
}
int esp_rom_spiflash_write(uint32_t off,const uint32_t *data,int32_t n) {
    assert(irq&&n>0&&!(off&3)&&!((uintptr_t)data&3)&&!(n&3)&&off>=0xc00000&&off+(unsigned)n<=0xd00000);
    for(int i=0;i<n;i++){assert((flash[off+(unsigned)i]&((const uint8_t *)data)[i])==((const uint8_t *)data)[i]);flash[off+(unsigned)i]&=((const uint8_t *)data)[i];}
    writes++;return 0;
}
int esp_rom_spiflash_erase_sector(uint32_t sector) {
    uint32_t off=sector*4096;assert(irq&&off>=0xc00000&&off+4096<=0xd00000);memset(flash+off,255,4096);erases++;return 0;
}
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static void partition(unsigned index,const char *name,unsigned sub,uint32_t off,uint32_t size) {
    uint8_t *p=flash+0x8000+index*32;memset(p,0,32);p[0]=0xaa;p[1]=0x50;p[2]=1;p[3]=sub;put32(p+4,off);put32(p+8,size);memcpy(p+12,name,strlen(name));
}
static int cmd(const char *action) {char *args[]={"programs",(char *)action};return programs_command(2,args);}
int main(void) {
    memset(flash,255,sizeof flash);partition(0,"wasmapps",0x41,0xa00000,0x200000);
    assert(!cmd("status")&&!reads&&!writes&&!erases);assert(cmd("init")==1&&!writes&&!erases);
    partition(1,"programs",0x42,0xc00000,PROGRAM_STORE_BYTES);
    flash[0xa00000]=0x55;flash[0x400000]=0x66;flash[0x3f0000]=0x77;flash[0x10000]=0x88;
    assert(cmd("mount")==1&&!writes&&!erases);assert(!cmd("init")&&program_fs_ready()&&writes==2&&!erases);
    unsigned before=reads;assert(cmd("mount")==1&&reads==before); /* already mounted */
    void *file;assert(!fs->open(NULL,"/bas/hello.bas",VFS_O_WRITE|VFS_O_CREATE,&file));
    assert(fs->write(file,"10 PRINT 1\n",11)==11);assert(!fs->close(file)&&erases==5);
    assert(!cmd("unmount"));writes=erases=0;assert(!cmd("mount")&&!writes&&!erases);
    assert(!fs->open(NULL,"/bas/hello.bas",VFS_O_READ,&file));char out[32];assert(fs->read(file,out,sizeof out)==11&&!memcmp(out,"10 PRINT 1\n",11));assert(!fs->close(file));
    assert(!cmd("unmount"));assert(flash[0xa00000]==0x55&&flash[0x400000]==0x66&&flash[0x3f0000]==0x77&&flash[0x10000]==0x88);
    /* Wrong region, encrypted table entries or small flash prevent writes. */
    put32(flash+0x8000+32+4,0xa00000);assert(cmd("init")==1&&!writes&&!erases);put32(flash+0x8000+32+4,0xc00000);
    put32(flash+0x8000+32+28,1);assert(cmd("init")==1&&!writes&&!erases);put32(flash+0x8000+32+28,0);
    g_rom_flashchip.chip_size=0xc00000;assert(cmd("init")==1&&!writes&&!erases);
    puts("programs ROM-adapter boundary/read-only-mount tests PASS");
}
