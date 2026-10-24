#pragma once
#include <stdint.h>
enum quake_phase { QP_COMMANDS, QP_SERVER, QP_CLIENT, QP_RENDER, QP_AUDIO, QP_COUNT };
enum quake_io_kind { QIO_OPEN, QIO_READ, QIO_SEEK, QIO_COUNT };
typedef struct {
    uint64_t phase_us[QP_COUNT], io_us[QIO_COUNT], read_bytes;
    unsigned io_calls[QIO_COUNT], completed_frames;
} quake_profile_stats;
void quake_profile_reset(void);
void quake_profile_mark(unsigned phase);
uint64_t quake_profile_clock(void);
void quake_profile_io(unsigned kind, uint64_t since, unsigned bytes);
quake_profile_stats quake_profile_take(void);
#ifdef MEOS_QUAKE_PROFILE
#define QUAKE_PHASE(p) quake_profile_mark(p)
#define QUAKE_IO_BEGIN uint64_t quake_io_since=quake_profile_clock()
#define QUAKE_IO_END(kind,bytes) quake_profile_io(kind,quake_io_since,bytes)
#else
#define QUAKE_PHASE(p) ((void)0)
#define QUAKE_IO_BEGIN ((void)0)
#define QUAKE_IO_END(kind,bytes) ((void)0)
#endif
