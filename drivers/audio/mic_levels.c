#include "mic_internal.h"
bool mic_levels_add(mic_levels_t *s, const int16_t *pcm, size_t bytes)
{
    if (!s || !pcm || !bytes || bytes > MIC_BLOCK_BYTES || bytes % (MIC_SLOTS * sizeof(int16_t)))
        return false;
    unsigned frames = bytes / (MIC_SLOTS * sizeof(int16_t));
    if (s->frames > UINT32_MAX - frames) return false;
    for (unsigned i = 0; i < frames; i++) for (unsigned ch = 0; ch < MIC_SLOTS; ch++) {
        int32_t sample = pcm[i * MIC_SLOTS + ch];
        uint32_t a = sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
        if (a > s->peak[ch]) s->peak[ch] = a;
        s->absolute_sum[ch] += a;
        if (sample == INT16_MIN || sample == INT16_MAX) s->clipped[ch]++;
    }
    s->frames += frames; return true;
}
