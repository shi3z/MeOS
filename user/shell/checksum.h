#pragma once
/* Read a regular file through VFS, reporting CRC32/size/time after close. */
int shell_crc32_file(const char *path);
