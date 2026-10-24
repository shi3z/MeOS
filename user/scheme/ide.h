#pragma once
#include <stdbool.h>
/* Absolute VFS path; caller remains in its task. Tab5 only.
 * True after a normal user exit; false if startup/load fails. */
bool scheme_ide(const char *path);
