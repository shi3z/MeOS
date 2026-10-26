#pragma once
#include <m5os/program_store.h>
#include <m5os/vfs.h>
/* Explicit mount only; never invoked by boot. init refuses nonblank media. */
int program_fs_mount(const program_store_io *);
int program_fs_init(const program_store_io *);
int program_fs_unmount(void);
bool program_fs_mounted(void);
bool program_fs_ready(void);
unsigned program_fs_recovered(void);
int programs_command(int argc,char **argv); /* Tab5 partition-checked ROM adapter */
