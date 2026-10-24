#pragma once
#include <m5os/types.h>
extern uint32_t quake_owner;
extern char quake_basedir[96];
void quake_exit(int code) __attribute__((noreturn));
void quake_io_close_all(void);

const char *quake_storage_base(void);
