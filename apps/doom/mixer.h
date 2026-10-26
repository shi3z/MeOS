#pragma once
#include <m5os/types.h>
#define DOOM_SFX_CHANNELS 8
void doom_mix_reset(void);
bool doom_mix_sfx_start(unsigned channel, const uint8_t *dmx, size_t length, int volume, int separation);
void doom_mix_sfx_stop(unsigned channel);
bool doom_mix_sfx_playing(unsigned channel);
void doom_mix_sfx_params(unsigned channel, int volume, int separation);
bool doom_mix_music_register(const uint8_t *mus, size_t length);
void doom_mix_music_play(bool loop);
void doom_mix_music_stop(void);
void doom_mix_music_pause(bool paused);
void doom_mix_music_volume(int volume);
bool doom_mix_music_playing(void);
void doom_mix_render(int16_t *stereo, unsigned frames, void *arg);
