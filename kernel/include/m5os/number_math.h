#pragma once
#include <m5os/number.h>
#define NUMBER_PI 3.14159265358979323846
enum { NUMBER_SIN, NUMBER_COS, NUMBER_TAN, NUMBER_SQRT, NUMBER_LOG,
       NUMBER_LOG10, NUMBER_EXP, NUMBER_ABS, NUMBER_INT, NUMBER_FIX };
/* Shared checked functions. LOG is natural; LOG10 is base ten. Input/output
 * obey number_valid. No heap or stdio. Results are written only on success. */
const char *number_math(int operation, double value, int degrees, double *result);
/* Real powers of positive bases; negative bases require integral exponents
 * -1000..1000. 0^0 and negative powers of zero are rejected. */
const char *number_power(double base, double exponent, double *result);
