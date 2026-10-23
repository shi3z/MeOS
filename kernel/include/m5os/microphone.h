#pragma once
#include <m5os/types.h>
/* Explicit bounded diagnostic capture only; no startup/background recording. */
int microphone_command(int argc, char **argv);
int microphone_app_cleanup(uint32_t app_id); /* ERR_BUSY retains owner memory for retry */
