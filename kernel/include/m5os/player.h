#pragma once
#include <m5os/types.h>
bool mp3_is_name(const char *name);
/* Kernel foreground UI: opens a native app, waits for its cleanup, then returns. */
int mp3_play_file(const char *path);
int mp3_app_cleanup(uint32_t app_id);
