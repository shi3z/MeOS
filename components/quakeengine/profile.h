#pragma once
#include <stdint.h>
enum quake_phase { QP_COMMANDS, QP_SERVER, QP_CLIENT, QP_RENDER, QP_AUDIO, QP_COUNT };
enum quake_io_kind { QIO_OPEN, QIO_READ, QIO_SEEK, QIO_COUNT };
enum quake_cache_event { QC_HIT, QC_REBUILD, QC_ALLOC, QC_EVICT, QC_WRAP,
    QC_THRASH, QC_DYNAMIC, QC_TEXTURE, QC_LIGHTSTYLE, QC_PIXELS, QC_BYTES, QC_COUNT };
/* Inclusive wall-time scopes: edges contains cache/spans; cache contains
 * build, and build contains light. These must not be added together. */
enum quake_detail { QD_SETUP,QD_EDGES,QD_ENTITIES,QD_VIEWMODEL,QD_PARTICLES,QD_WARP,
    QD_CACHE,QD_BUILD,QD_LIGHT,QD_SPANS,QD_ZSPANS,QD_SKY,QD_WATER,QD_COUNT };
typedef struct {
    uint64_t phase_us[QP_COUNT], io_us[QIO_COUNT], read_bytes;
    unsigned io_calls[QIO_COUNT], completed_frames;
    uint64_t cache[QC_COUNT];
    uint64_t detail_us[QD_COUNT];
    unsigned detail_calls[QD_COUNT];
} quake_profile_stats;
void quake_profile_reset(void);
void quake_profile_mark(unsigned phase);
uint64_t quake_profile_clock(void);
void quake_profile_io(unsigned kind, uint64_t since, unsigned bytes);
quake_profile_stats quake_profile_take(void);
void quake_profile_cache(unsigned event,unsigned amount);
void quake_profile_detail(unsigned kind,uint64_t began);
#ifdef MEOS_QUAKE_DETAIL_PROFILE
#define QUAKE_DETAIL_BEGIN(name) uint64_t name=quake_profile_clock()
#define QUAKE_DETAIL_END(kind,name) quake_profile_detail(kind,name)
#else
#define QUAKE_DETAIL_BEGIN(name) ((void)0)
#define QUAKE_DETAIL_END(kind,name) ((void)0)
#endif
#ifdef MEOS_QUAKE_PROFILE
#define QUAKE_PHASE(p) quake_profile_mark(p)
#define QUAKE_CACHE(e,n) quake_profile_cache(e,n)
#define QUAKE_IO_BEGIN uint64_t quake_io_since=quake_profile_clock()
#define QUAKE_IO_END(kind,bytes) quake_profile_io(kind,quake_io_since,bytes)
#else
#define QUAKE_PHASE(p) ((void)0)
#define QUAKE_CACHE(e,n) ((void)0)
#define QUAKE_IO_BEGIN ((void)0)
#define QUAKE_IO_END(kind,bytes) ((void)0)
#endif
