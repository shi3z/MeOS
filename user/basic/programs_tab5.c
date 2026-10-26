#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/program_fs.h>
#include <m5os/arch.h>
#include <m5os/task.h>
#include <m5os/kprintf.h>
#include <m5os/errno.h>
#include "esp_attr.h"
#include "esp_rom_spiflash.h"
#include <string.h>

extern esp_rom_spiflash_result_t esp_rom_spiflash_unlock(void);
static uint32_t base;
/* ROM calls receive only aligned internal stack buffers while cache/IRQs may
 * be unavailable; copying to/from PSRAM happens after ROM has returned. */
static bool IRAM_ATTR rom_read(uint32_t address,uint32_t *out,size_t n)
{
    uint32_t f=arch_irq_disable();
    esp_rom_spiflash_result_t rc=esp_rom_spiflash_read(address,out,n);
    arch_irq_restore(f);return rc==ESP_ROM_SPIFLASH_RESULT_OK;
}
static bool IRAM_ATTR rom_write(uint32_t address,const uint32_t *data,size_t n)
{
    uint32_t f=arch_irq_disable();
    esp_rom_spiflash_result_t rc=esp_rom_spiflash_unlock();
    if(rc==ESP_ROM_SPIFLASH_RESULT_OK) rc=esp_rom_spiflash_write(address,data,n);
    arch_irq_restore(f);return rc==ESP_ROM_SPIFLASH_RESULT_OK;
}
static bool IRAM_ATTR rom_erase(uint32_t address)
{
    uint32_t f=arch_irq_disable();
    esp_rom_spiflash_result_t rc=esp_rom_spiflash_unlock();
    if(rc==ESP_ROM_SPIFLASH_RESULT_OK) rc=esp_rom_spiflash_erase_sector(address/4096);
    arch_irq_restore(f);return rc==ESP_ROM_SPIFLASH_RESULT_OK;
}
static int bounded(uint32_t off,size_t n) {return base==0xc00000u && off<=PROGRAM_STORE_BYTES && n<=PROGRAM_STORE_BYTES-off;}
static int read_flash(void *ctx,uint32_t off,void *out,size_t length)
{
    (void)ctx;if(!bounded(off,length)) return ERR_INVAL;
    uint8_t *dst=out;
    while(length) {
        uint32_t words[64];uint32_t address=base+off;unsigned skip=address&3u;
        size_t n=sizeof words-skip;if(n>length) n=length;
        if(!rom_read(address&~3u,words,(n+skip+3u)&~(size_t)3)) return ERR_IO;
        memcpy(dst,(uint8_t *)words+skip,n);dst+=n;off+=(uint32_t)n;length-=n;
    }
    return ERR_OK;
}
static int write_flash(void *ctx,uint32_t off,const void *data,size_t length)
{
    (void)ctx;if(!bounded(off,length) || (off&3u) || (length&3u)) return ERR_INVAL;
    const uint8_t *src=data;
    while(length) {
        uint32_t words[64];size_t n=length<sizeof words?length:sizeof words;
        memcpy(words,src,n);
        if(!rom_write(base+off,words,n)) return ERR_IO;
        src+=n;off+=(uint32_t)n;length-=n;
    }
    return ERR_OK;
}
static int erase_flash(void *ctx,uint32_t off)
{
    (void)ctx;if(!bounded(off,4096)||(off&4095u)) return ERR_INVAL;
    if(!rom_erase(base+off)) return ERR_IO;
    task_sleep(1);return ERR_OK;
}
static int probe(void)
{
    uint8_t table[4096];
    for(unsigned off=0;off<sizeof table;off+=256) {
        uint32_t words[64];if(!rom_read(0x8000u+off,words,sizeof words)) return ERR_IO;
        memcpy(table+off,words,sizeof words);
    }
    uint32_t checked;int rc=program_store_partition(table,g_rom_flashchip.chip_size,&checked);
    if(!rc) base=checked;else base=0;
    return rc;
}
int programs_command(int argc,char **argv)
{
    if(argc==1 || (argc==2&&!strcmp(argv[1],"status"))) {
        kprintf("data: %s; %s; %u recovered slot(s)\n",program_fs_mounted()?"mounted at /data":"unmounted",
                program_fs_ready()?"ready":"not ready",program_fs_recovered());
        kputs("Opt-in data store partition required; mount does not write flash; init writes only blank media.\n");return 0;
    }
    int rc;
    if(argc==2&&!strcmp(argv[1],"unmount")) rc=program_fs_unmount();
    else if(argc==2&&(!strcmp(argv[1],"mount")||!strcmp(argv[1],"init"))) {
        rc=program_fs_mounted()?ERR_BUSY:probe();
        if(!rc) {
            const program_store_io io={.read=read_flash,.write=write_flash,.erase=erase_flash};
            rc=program_fs_mount(&io);
            if(rc==ERR_NOENT&&!strcmp(argv[1],"init")) {rc=program_fs_init(&io);if(!rc) rc=program_fs_mount(&io);}
        }
    } else {kputs("usage: data [status|mount|init|unmount]\n");return 1;}
    if(rc<0) kprintf("data: %s failed (%d); no auto-format; close files and remount after a failed save\n",argv[1],rc);
    else kprintf("data: %s ok\n",argv[1]);
    return rc<0?1:0;
}
#endif
