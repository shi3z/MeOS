#include <assert.h>
#include <stdio.h>
#include "../../components/quakeengine/profile.h"
static uint64_t now;
uint64_t hal_time_now_us(void){return now;}
int main(void){
    quake_profile_reset();
    quake_profile_mark(QP_COMMANDS);now=10;quake_profile_mark(QP_SERVER);
    now=30;quake_profile_mark(QP_CLIENT);now=60;quake_profile_mark(QP_RENDER);
    now=100;quake_profile_mark(QP_AUDIO);now=150;quake_profile_mark(QP_COUNT);
    quake_profile_io(QIO_READ,100,1234);quake_profile_io(QIO_OPEN,140,0);
    quake_profile_stats s=quake_profile_take();assert(s.completed_frames==1);
    for(unsigned i=0;i<QP_COUNT;i++)assert(s.phase_us[i]==10*(i+1));
    assert(s.read_bytes==1234&&s.io_calls[QIO_READ]==1&&s.io_us[QIO_READ]==50);
    assert(s.io_calls[QIO_OPEN]==1&&s.io_us[QIO_OPEN]==10);
    s=quake_profile_take();assert(!s.completed_frames&&!s.read_bytes);
    /* Aborted host frame: next begin must not charge the idle gap. */
    quake_profile_mark(QP_COMMANDS);now=160;quake_profile_mark(QP_SERVER);
    now=1000;quake_profile_mark(QP_COMMANDS);now=1005;quake_profile_mark(QP_COUNT);
    s=quake_profile_take();assert(s.phase_us[QP_COMMANDS]==15&&!s.phase_us[QP_SERVER]);
    assert(s.completed_frames==1);
    quake_profile_reset();s=quake_profile_take();assert(!s.phase_us[QP_COMMANDS]);
    puts("Quake profiler: phase accounting, independent IO, interval reset and aborted-frame gap PASS");
}
