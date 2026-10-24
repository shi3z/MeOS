#pragma once
#include <m5os/types.h>
int doom_command(int argc, char **argv);
void doom_main(void *arg);
void doom_app_cleanup(uint32_t app_id);
