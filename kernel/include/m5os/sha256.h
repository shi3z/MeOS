#pragma once
#include <m5os/types.h>
/* SHA-256 (FIPS 180-4), for checking a downloaded app against the hash in its QR code. */
void sha256(const void *data, size_t len, uint8_t out[32]);
