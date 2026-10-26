#include <m5os/sdcard.h>
#include <m5os/fat.h>
#include <m5os/sdroot.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static bool root, fat;
static int probes, mounts, benchmarks, stats_calls, resets;
bool sdroot_active(void){return root;}
bool fat_is_mounted(void){return fat;}
bool fat_is_readonly(void){return false;}
void sdcard_status(sdcard_status_t *s){*s=(sdcard_status_t){.detail="test",.ready=true,.sectors=65536};}
bool sdcard_get_io_stats(sdcard_io_stats_t *s,bool reset){*s=(sdcard_io_stats_t){0};stats_calls++;resets+=reset;return true;}
blockdev_t *sdcard_probe(void){static blockdev_t bd;probes++;return &bd;}
int fat_mount(blockdev_t *bd,bool ro){(void)bd;(void)ro;mounts++;fat=true;return 0;}
int fat_unmount(void){fat=false;return 0;}
int sd_benchmark(unsigned files,unsigned small,unsigned large){(void)files;(void)small;(void)large;benchmarks++;return 0;}
void kprintf(const char *fmt,...){(void)fmt;}
void kputs(const char *s){(void)s;}
int shell_sd(int,char **);
int main(void){
    char *probe[]={"sd","probe"},*mount[]={"sd","mount"},*bench[]={"sd","bench"},*status[]={"sd","status"},*unmount[]={"sd","unmount"},*rw[]={"sd","mount","rw"};
    root=true;assert(shell_sd(2,probe));assert(shell_sd(2,mount));assert(shell_sd(2,bench));
    char *stats[]={"sd","stats"},*reset[]={"sd","stats","reset"},*bad[]={"sd","stats","probe"};
    assert(!shell_sd(2,stats)&&!shell_sd(3,reset));
    assert(shell_sd(3,bad)&&stats_calls==2&&resets==1);
    assert(shell_sd(2,unmount));assert(shell_sd(3,rw));
    assert(!shell_sd(2,status));assert(!probes&&!mounts&&!benchmarks);
    root=false;assert(!shell_sd(2,probe));assert(probes==1);
    assert(!shell_sd(2,mount));assert(probes==2&&mounts==1);
    puts("SD root/FAT probe, mount and benchmark exclusion PASS");
}
