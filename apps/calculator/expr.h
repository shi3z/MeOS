#pragma once
#include <stddef.h>

/* Bounded binary64 evaluator. No allocation, libc I/O or hardware float API.
 * Range: zero or magnitude 1e-100..1e100; display: 12 significant digits.
 * ^ is right associative; -2^2 == -4; exponents must be integral, |n| <= 1000.
 * Result is only written on success. Error position is a zero-based byte offset. */
typedef struct {
    double ans;
    int has_ans;
    int degrees; /* nonzero: DEG; zero: RAD */
} calc_context;
/* Successful evaluation updates Ans; errors preserve it. Names are case insensitive.
 * sin/cos/tan, sqrt, log (base 10), ln, exp, abs; pi and e constants.
 * Trig arguments are bounded to +/-1e9 radians; near-pole tan is rejected. */
const char *calc_eval_context(const char *text, calc_context *context,
                              double *result, size_t *error_pos);
/* Stateless evaluation: RAD, no previous Ans. */
const char *calc_eval(const char *text, double *result, size_t *error_pos);
/* Requires at least 24 bytes. Returns 0 for invalid values or insufficient space. */
int calc_format(double value, char *out, size_t cap);
