#pragma once
#include <m5os/types.h>
#define AUDIO_RATE 24000u
typedef void (*audio_render_t)(int16_t *stereo, unsigned frames, void *arg);
/* Single owner. Callback runs in a dedicated MeOS kernel task; must not block.
 * Close stops DMA before the owner releases any sample/callback memory. */
bool audio_open(uint32_t owner, audio_render_t render, void *arg);
void audio_close(uint32_t owner);
int audio_command(int argc, char **argv);
