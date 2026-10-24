#include <m5os/number_math.h>
#include <math.h>

static const char *store(double value, double *result)
{
    if (!number_valid(value)) return "numeric range exceeded";
    *result = value; return NULL;
}
const char *number_math(int op, double x, int degrees, double *result)
{
    if (!number_valid(x)) return "numeric range exceeded";
    switch (op) {
    case NUMBER_SIN: case NUMBER_COS: case NUMBER_TAN: {
        if (degrees) x *= NUMBER_PI / 180;
        if (x < -1e9 || x > 1e9) return "angle out of range";
        if (op == NUMBER_SIN) return store(sin(x), result);
        double c = cos(x);
        if (op == NUMBER_COS) return store(c, result);
        if (c > -1e-14 && c < 1e-14) return "tan is undefined near this angle";
        return store(sin(x) / c, result);
    }
    case NUMBER_SQRT:
        if (x < 0) return "sqrt requires a nonnegative argument";
        return store(sqrt(x), result);
    case NUMBER_LOG: case NUMBER_LOG10:
        if (x <= 0) return "log requires a positive argument";
        return store(op == NUMBER_LOG ? log(x) : log10(x), result);
    case NUMBER_EXP:
        if (x < -230.25850929940458 || x > 230.25850929940458) return "numeric range exceeded";
        return store(exp(x), result);
    case NUMBER_ABS: return store(x < 0 ? -x : x, result);
    case NUMBER_INT: return store(floor(x), result);
    case NUMBER_FIX: return store(trunc(x), result);
    default: return "unknown math operation";
    }
}
const char *number_power(double base, double exponent, double *result)
{
    if (!number_valid(base) || !number_valid(exponent)) return "numeric range exceeded";
    if (!base) {
        if (exponent <= 0) return "undefined power";
        *result = 0; return NULL;
    }
    if (base < 0) {
        int32_t integer;
        if (!number_int32(exponent, &integer) || integer < -1000 || integer > 1000)
            return "negative base requires an integer exponent in -1000..1000";
    }
    double magnitude = base < 0 ? -base : base;
    /* Guard before pow: bounded valid arguments cannot enter overflow/underflow
     * or domain-error branches in libm. Small slack accommodates log rounding. */
    double log_result = exponent * log(magnitude);
    if (log_result < -230.2585092994056 || log_result > 230.2585092994056)
        return "numeric range exceeded";
    return store(pow(base, exponent), result);
}
