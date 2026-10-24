#pragma once
#include <m5os/types.h>

/* Full-screen text editor in the Dragon-Quest window style (user/tui).  The whole file lives in one
 * buffer of EDIT_MAX bytes; a new file is created on the first save.  Returns true if the file was
 * changed on disk (so a caller can reload its listing).  Runs in the calling task until the user quits. */
#define EDIT_MAX 32768
bool edit_file(const char *path);

/* Start from caller's text instead of loading the file. Ctrl-S explicitly writes
 * to path; true means at least one save succeeded and the last save did not fail.
 * The caller retains its own buffer, including on cancel or a failed save. */
bool edit_file_with_text(const char *path, const char *text, size_t length);
