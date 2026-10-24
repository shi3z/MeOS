#include "sd.h"
#include "sdbench.h"
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/sdcard.h>
#include <m5os/fat.h>
#include <m5os/sdroot.h>
#include <m5os/kprintf.h>
#include <m5os/errno.h>
#include <string.h>

void shell_sd_status(void)
{
    sdcard_status_t s; sdcard_status(&s);
    kprintf("microSD: %s\n", s.detail);
    if (s.ready) kprintf("Card: %s, %u MiB (%u sectors)\n", s.high_capacity ? "SDHC/SDXC" : "SDSC", s.sectors / 2048u, s.sectors);
    if (s.probed) kprintf("Last card status: 0x%08x (low 0x%02x)\n",
                          s.last_status, s.last_r1);
    if(fat_is_mounted())kprintf("FAT: mounted %s at /sd (VFAT UTF-8 long names)\n",fat_is_readonly()?"read-only":"read/write");
    else kputs("FAT: not mounted\n");
    kprintf("Transport: %s", s.transport ? s.transport : "unknown");
    if (s.clock_khz) kprintf(", %u-bit @ %u kHz", s.bus_width, s.clock_khz);
    kputs("\n");
}
static bool number(const char *text,unsigned *out) {
    unsigned n=0;if(!*text)return false;
    for(;*text;text++){if(*text<'0'||*text>'9'||n>100000)return false;n=n*10+(unsigned)(*text-'0');}
    *out=n;return true;
}
int shell_sd(int argc, char **argv)
{
    if(sdroot_active() && !(argc==1 || (argc==2 && !strcmp(argv[1],"status")))) {
        kputs("sd: card is the active littlefs system root; raw probe/FAT mount/benchmark disabled\n");return 1;
    }
    if(argc>=2 && !strcmp(argv[1],"bench")){
        unsigned files=128,small=1024,large=8;
        if(argc>5 || (argc>2 && !number(argv[2],&files)) || (argc>3 && !number(argv[3],&small)) ||
            (argc>4 && !number(argv[4],&large)))goto usage;
        int rc=sd_benchmark(files,small,large);
        if(rc)kprintf("sd bench: failed (%d)\n",rc);
        return rc?1:0;
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) { shell_sd_status(); return 0; }
    bool readonly=false;
    if(argc==3 && !strcmp(argv[1],"mount")) {
        if(!strcmp(argv[2],"ro"))readonly=true;
        else if(strcmp(argv[2],"rw"))goto usage;
    } else if (argc != 2) goto usage;
    int rc;
    if (!strcmp(argv[1], "unmount")) {
        rc = fat_unmount();
        kprintf("sd: unmount %s (%d)\n", rc == ERR_OK ? "ok" : "failed", rc);
        return rc == ERR_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "probe") && strcmp(argv[1], "mount")) goto usage;
    if (fat_is_mounted()) { kputs("sd: already mounted; close files and use sd unmount before probing\n"); return 1; }
    blockdev_t *bd = sdcard_probe();
    shell_sd_status();
    if (!bd) return 1;
    if (!strcmp(argv[1], "probe")) return 0;
    sdcard_status_t status;sdcard_status(&status);
    if(!readonly && status.write_protected){kputs("sd: card write-protected; use sd mount ro\n");return 1;}
    rc = fat_mount(bd,readonly);
    if (rc == ERR_OK) kprintf("sd: mounted /sd %s; close files and sd unmount before removal\n",readonly?"read-only":"read/write");
    else kprintf("sd: FAT mount failed (%d); unsupported/damaged filesystem or read error; no formatting\n", rc);
    return rc == ERR_OK ? 0 : 1;
usage:
    kputs("usage: sd [status|probe|mount [ro|rw]|unmount|bench [FILES [SMALL_BYTES [LARGE_MiB]]]] (default read/write; FAT12/16/32, UTF-8 long names; no exFAT)\n");
    return 1;
}
#endif
