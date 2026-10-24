#pragma once
#include <m5os/types.h>
extern uint32_t doom_owner;
extern uint8_t *doom_iwad;
extern size_t doom_iwad_size;
extern char doom_iwad_path[96];
void doom_input_reset(void);
void doom_poll_inputs(void);
void doom_printf_frame_stats(void);
void doom_exit(int code) __attribute__((noreturn));
void doom_sound_reset(void);
void doom_sound_shutdown(void);
void doom_profile_input(uint32_t elapsed_us);
extern uint16_t doom_video_palette[256];
