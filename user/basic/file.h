#pragma once
#include "basic.h"
/* VFS helpers accept resolved absolute paths. LOAD stages all source and only
 * commits after complete read, close and structural validation. On failure,
 * current source/variables remain intact. SAVE overwrites and is not atomic:
 * VFS currently has no rename/transaction API; failures may leave a partial file. */
typedef struct { const char *detail; unsigned line; size_t bytes; } basic_file_result;
int basic_file_load(basic_session *, const char *path, basic_file_result *);
int basic_file_save(basic_session *, const char *path, basic_file_result *);
