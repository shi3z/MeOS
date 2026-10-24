#pragma once
/* End-to-end VFS benchmark; creates a fresh /sd directory, verifies and removes
 * only its own files. Errors leave artifacts for inspection. No format/remount. */
int sd_benchmark(unsigned files, unsigned small_bytes, unsigned large_mib);
