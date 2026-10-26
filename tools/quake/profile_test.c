#include <assert.h>
#include <stdio.h>
#include "../../components/quakeengine/profile.h"
static uint64_t now;
static unsigned clock_reads;
uint64_t hal_time_now_us(void){clock_reads++;return now;}
int main(void){
    quake_profile_reset();
    quake_profile_mark(QP_COMMANDS);now=10;quake_profile_mark(QP_SERVER);
    now=30;quake_profile_mark(QP_CLIENT);now=60;quake_profile_mark(QP_RENDER);
    now=100;quake_profile_mark(QP_AUDIO);now=150;quake_profile_mark(QP_COUNT);
    quake_profile_io(QIO_READ,100,1234);quake_profile_io(QIO_OPEN,140,0);
    quake_profile_cache(QC_HIT,3);quake_profile_cache(QC_PIXELS,2048);quake_profile_cache(QC_PIXELS,1024);
    quake_profile_stats s=quake_profile_take();assert(s.completed_frames==1);
    for(unsigned i=0;i<QP_COUNT;i++)assert(s.phase_us[i]==10*(i+1));
    assert(s.read_bytes==1234&&s.io_calls[QIO_READ]==1&&s.io_us[QIO_READ]==50);
    assert(s.io_calls[QIO_OPEN]==1&&s.io_us[QIO_OPEN]==10);
    assert(s.cache[QC_HIT]==3&&s.cache[QC_PIXELS]==3072);
    s=quake_profile_take();assert(!s.completed_frames&&!s.read_bytes&&!s.cache[QC_HIT]&&!s.cache[QC_PIXELS]);
    /* Aborted host frame: next begin must not charge the idle gap. */
    quake_profile_mark(QP_COMMANDS);now=160;quake_profile_mark(QP_SERVER);
    now=1000;quake_profile_mark(QP_COMMANDS);now=1005;quake_profile_mark(QP_COUNT);
    s=quake_profile_take();assert(s.phase_us[QP_COMMANDS]==15&&!s.phase_us[QP_SERVER]);
    assert(s.completed_frames==1);
    quake_profile_reset();s=quake_profile_take();assert(!s.phase_us[QP_COMMANDS]);
    /* Inclusive, independently timed nested scopes and reset. Invalid tags
     * neither read the clock nor index the arrays. */
    now=1020;quake_profile_detail(QD_CACHE,1000);quake_profile_detail(QD_LIGHT,1015);
    unsigned reads=clock_reads;quake_profile_detail(QD_COUNT,0);assert(clock_reads==reads);
    s=quake_profile_take();assert(s.detail_us[QD_CACHE]==20&&s.detail_us[QD_LIGHT]==5);
    assert(s.detail_calls[QD_CACHE]==1&&s.detail_calls[QD_LIGHT]==1);
    s=quake_profile_take();assert(!s.detail_us[QD_CACHE]&&!s.detail_calls[QD_LIGHT]);
    reads=clock_reads;QUAKE_DETAIL_BEGIN(scope);now+=7;QUAKE_DETAIL_END(QD_SPANS,scope);
    s=quake_profile_take();
#ifdef MEOS_QUAKE_DETAIL_PROFILE
    assert(clock_reads==reads+2&&s.detail_us[QD_SPANS]==7&&s.detail_calls[QD_SPANS]==1);
#else
    assert(clock_reads==reads&&!s.detail_us[QD_SPANS]&&!s.detail_calls[QD_SPANS]);
#endif
    puts("Quake profiler: phase accounting, independent IO, interval reset and aborted-frame gap PASS");
}
