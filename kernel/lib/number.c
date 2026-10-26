#include <m5os/number.h>
#include <limits.h>
#include <float.h>

_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "MeOS number conversion requires binary64 double");

#define LIMIT 1.00000000000001e100
#define FLOOR 0.99999999999999e-100
int number_valid(double value)
{
    double a = value < 0 ? -value : value;
    return a <= LIMIT && (!a || a >= FLOOR);
}
int number_int32(double value, int32_t *out)
{
    if (!(value >= INT32_MIN && value <= INT32_MAX)) return 0;
    int32_t n = (int32_t)value;
    if ((double)n != value) return 0;
    *out = n; return 1;
}
static int digit(char c) { return c >= '0' && c <= '9'; }
const char *number_parse(const char *text, const char **end, double *out)
{
    const char *p = text;
    int negative = 0, dot = 0, any = 0, nonzero = 0, stored = 0, scale = 0;
    double significand = 0;
    if (*p == '+' || *p == '-') negative = *p++ == '-';
    while (digit(*p) || (!dot && *p == '.')) {
        if (*p == '.') { dot = 1; p++; continue; }
        unsigned d = *p++ - '0'; any = 1;
        if (!nonzero && !d) { if (dot) scale--; }
        else {
            nonzero = 1;
            if (stored < 17) { significand = significand * 10 + d; stored++; if (dot) scale--; }
            else if (!dot) scale++;
        }
        if (scale < -4096 || scale > 4096) { *end = p; return "numeric literal too long"; }
    }
    if (!any) { *end = p; return "expected number"; }
    if (*p == 'e' || *p == 'E') {
        p++; int sign = 1, exponent = 0;
        if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }
        if (!digit(*p)) { *end = p; return "expected exponent digits"; }
        while (digit(*p)) {
            exponent = exponent * 10 + (*p++ - '0');
            if (exponent > 4096) { *end = p; return "exponent out of range"; }
        }
        scale += sign * exponent;
    }
    *end = p;
    if (!nonzero) { *out = 0; return NULL; }
    int exponent = scale + stored - 1;
    if (exponent < -100 || exponent > 100) return "numeric range exceeded";
    while (scale > 0) { significand *= 10; scale--; }
    while (scale < 0) { significand /= 10; scale++; }
    double result = negative ? -significand : significand;
    if (!number_valid(result)) return "numeric range exceeded";
    *out = result; return NULL;
}
int number_format(double x, char *out, size_t cap)
{
    if (cap < 24) return 0;
    if (!number_valid(x)) return 0;
    double a = x < 0 ? -x : x;
    int pos = 0, exponent = 0;
    if (x < 0) out[pos++] = '-';
    if (!a) { out[0] = '0'; out[1] = 0; return 1; }
    while (a >= 10) { a /= 10; exponent++; }
    while (a < 1) { a *= 10; exponent--; }
    uint64_t rounded = (uint64_t)(a * 100000000000.0 + 0.5);
    if (rounded >= 1000000000000ULL) { rounded /= 10; exponent++; }
    char digits[12];
    for (int i = 11; i >= 0; i--) { digits[i] = '0' + rounded % 10; rounded /= 10; }
    int last = 11; while (last && digits[last] == '0') last--;
    if (exponent >= -4 && exponent < 12) {
        if (exponent < 0) { out[pos++] = '0'; out[pos++] = '.'; for (int i = -1; i > exponent; i--) out[pos++] = '0'; }
        int end = last > exponent ? last : exponent;
        for (int i = 0; i <= end; i++) { out[pos++] = i <= last ? digits[i] : '0'; if (i == exponent && i < last) out[pos++] = '.'; }
    } else {
        out[pos++] = digits[0]; if (last) out[pos++] = '.';
        for (int i = 1; i <= last; i++) out[pos++] = digits[i];
        out[pos++] = 'e'; out[pos++] = exponent < 0 ? '-' : '+';
        if (exponent < 0) exponent = -exponent;
        if (exponent >= 100) out[pos++] = '0' + exponent / 100;
        if (exponent >= 10) out[pos++] = '0' + exponent / 10 % 10;
        out[pos++] = '0' + exponent % 10;
    }
    out[pos] = 0; return pos;
}
