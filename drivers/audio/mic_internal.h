#pragma once
#include <m5os/types.h>
#define MIC_SLOTS 4u
#define MIC_RATE 48000u
#define MIC_BLOCK_FRAMES 256u
#define MIC_BLOCK_BYTES (MIC_BLOCK_FRAMES * MIC_SLOTS * sizeof(int16_t))
typedef struct {
    uint32_t frames;
    uint32_t peak[MIC_SLOTS], clipped[MIC_SLOTS];
    uint64_t absolute_sum[MIC_SLOTS];
} mic_levels_t;
bool mic_levels_add(mic_levels_t *levels, const int16_t *pcm, size_t bytes);
bool mic_codec_start(void);
bool mic_codec_stop(void);
