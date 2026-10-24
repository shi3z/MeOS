#pragma once
#include <stddef.h>
#include <stdint.h>
/* Shared bounded binary64 decimal conversion. No allocation, stdio, locale or
 * errno. Nonzero magnitudes approximately 1e-100..1e100; output 12 significant
 * digits. Normal binary rounding applies; this is not a decimal arithmetic API. */
int number_valid(double value);
/* Consume one signed decimal literal with optional dot and e/E exponent.
 * Always sets end; writes value only on success (NULL error). No whitespace. */
const char *number_parse(const char *text, const char **end, double *value);
/* Strict checked conversion: rejects fractions, NaN/Inf and out-of-int32 range. */
int number_int32(double value, int32_t *out);
/* Requires >=24 bytes; returns stored length, or 0 on invalid value/capacity. */
int number_format(double value, char *out, size_t capacity);
