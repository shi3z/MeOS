#include "sd.h"
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/sdcard.h>
#include <m5os/fat.h>
#include <m5os/kprintf.h>
#include <m5os/errno.h>
#include <string.h>

void shell_sd_status(void)
{
    sdcard_status_t s; sdcard_status(&s);
    kprintf("microSD: %s\n", s.detail);
    if (s.ready) kprintf("Card: %s, %u MiB (%u sectors)\n", s.high_capacity ? "SDHC/SDXC" : "SDSC", s.sectors / 2048u, s.sectors);
    if (s.probed) kprintf("Last R1: 0x%02x\n", s.last_r1);
    if(fat_is_mounted())kprintf("FAT: mounted %s at /sd (VFAT UTF-8 long names)\n",fat_is_readonly()?"read-only":"read/write");
    else kputs("FAT: not mounted\n");
    kputs("Transport: dedicated SPI pins; Wi-Fi SDMMC registers unchanged\n");
}
int shell_sd(int argc, char **argv)
{
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
    kputs("usage: sd [status|probe|mount [ro|rw]|unmount] (default read/write; FAT12/16/32, UTF-8 long names; no exFAT)\n");
    return 1;
}
#endif
