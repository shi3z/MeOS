#include <m5os/board_config.h>
#if M5OS_QUAKE
#include <m5os/hal_time.h>
#include "../../components/quakeengine/profile.h"
#include <string.h>
static quake_profile_stats stats;
static unsigned current=QP_COUNT;
static uint64_t since;
uint64_t quake_profile_clock(void){return hal_time_now_us();}
void quake_profile_reset(void){memset(&stats,0,sizeof stats);current=QP_COUNT;since=0;}
void quake_profile_mark(unsigned phase){
    uint64_t now=quake_profile_clock();
    /* A new command phase also discards an interrupted frame's pending span.
     * Never charge idle time after a host_abortserver longjmp to that span. */
    if(phase!=QP_COMMANDS&&current<QP_COUNT)stats.phase_us[current]+=now-since;
    if(phase==QP_COUNT&&current<QP_COUNT)stats.completed_frames++;
    current=phase;since=now;
}
void quake_profile_io(unsigned kind,uint64_t began,unsigned bytes){
    if(kind>=QIO_COUNT)return;
    stats.io_us[kind]+=quake_profile_clock()-began;stats.io_calls[kind]++;
    if(kind==QIO_READ)stats.read_bytes+=bytes;
}
quake_profile_stats quake_profile_take(void){
    quake_profile_stats result=stats;memset(&stats,0,sizeof stats);return result;
}
#endif
