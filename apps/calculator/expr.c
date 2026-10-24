#include "expr.h"
#include <stdint.h>
#include <m5os/number.h>
#include <m5os/number_math.h>

#define PI NUMBER_PI

#define DEPTH 16

typedef struct { const char *start, *p, *error; const char *message; unsigned depth; calc_context *context; } parser;
static double expression(parser *p);
static void space(parser *p) { while (*p->p == ' ' || *p->p == '\t') p->p++; }
static void fail(parser *p, const char *s) { if (!p->message) { p->message = s; p->error = p->p; } }
static double checked(parser *p, double x)
{
    if (!number_valid(x)) fail(p, "numeric range exceeded");
    return p->message ? 0 : x;
}
static int letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int named(const char *name, const char *expected)
{
    while (*name && *expected && *name == *expected) { name++; expected++; }
    return !*name && !*expected;
}
static double identifier(parser *p)
{
    char name[8]; unsigned n = 0;
    while (letter(*p->p)) {
        char c = *p->p++;
        if (n + 1 >= sizeof name) { fail(p, "unknown name"); return 0; }
        name[n++] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    }
    name[n] = 0;
    if (named(name, "pi")) return PI;
    if (named(name, "e")) return 2.71828182845904523536;
    if (named(name, "ans")) {
        if (!p->context->has_ans) { fail(p, "Ans is not set"); return 0; }
        return checked(p, p->context->ans);
    }
    static const struct { const char *name; int op; } functions[] = {
        {"sin",NUMBER_SIN}, {"cos",NUMBER_COS}, {"tan",NUMBER_TAN},
        {"sqrt",NUMBER_SQRT}, {"log",NUMBER_LOG10}, {"ln",NUMBER_LOG},
        {"exp",NUMBER_EXP}, {"abs",NUMBER_ABS}
    };
    int operation = -1;
    for (unsigned i = 0; i < sizeof functions/sizeof *functions; i++)
        if (named(name, functions[i].name)) operation = functions[i].op;
    if (operation < 0) { fail(p, "unknown name"); return 0; }
    space(p);
    if (*p->p != '(') { fail(p, "expected '(' after function"); return 0; }
    p->p++;
    double x = expression(p); space(p);
    if (*p->p != ')') fail(p, "expected ')'"); else p->p++;
    if (p->message) return 0;
    double result = 0;
    const char *error = number_math(operation, x, p->context->degrees, &result);
    if (error) fail(p, error);
    return result;
}
static double primary(parser *p)
{
    space(p);
    if (letter(*p->p)) return identifier(p);
    if (*p->p == '(') {
        p->p++;
        double x = expression(p);
        space(p);
        if (*p->p != ')') fail(p, "expected ')'"); else p->p++;
        return x;
    }
    double x = 0;
    const char *end;
    const char *error = number_parse(p->p, &end, &x);
    p->p = end;
    if (error) fail(p, error);
    return checked(p, x);
}
static double unary(parser *p)
{
    if (++p->depth > DEPTH) { fail(p, "expression too deep"); p->depth--; return 0; }
    space(p);
    double x;
    if (*p->p == '+' || *p->p == '-') {
        char op = *p->p++;
        x = unary(p); if (op == '-') x = -x;
    } else {
        x = primary(p); space(p);
        if (!p->message && *p->p == '^') {
            p->p++;
            double e = unary(p);
            if (!p->message && (e < -1000 || e > 1000 || e != (int)e)) fail(p, "exponent must be an integer in -1000..1000");
            if (!p->message) {
                double result = 0;
                const char *error = number_power(x, e, &result);
                if (error) fail(p, error);
                x = result;
            }
        }
    }
    p->depth--;
    return x;
}
static double product(parser *p)
{
    double x = unary(p);
    space(p);
    while (!p->message && (*p->p == '*' || *p->p == '/')) {
        char op = *p->p++; double y = unary(p);
        if (p->message) break;
        if (op == '/' && y == 0) { fail(p, "division by zero"); break; }
        x = checked(p, op == '*' ? x * y : x / y); space(p);
    }
    return x;
}
static double expression(parser *p)
{
    double x = product(p); space(p);
    while (!p->message && (*p->p == '+' || *p->p == '-')) {
        char op = *p->p++; double y = product(p);
        if (p->message) break;
        x = checked(p, op == '+' ? x + y : x - y); space(p);
    }
    return x;
}
const char *calc_eval_context(const char *text, calc_context *context, double *result, size_t *error_pos)
{
    parser p = { .start = text, .p = text, .context = context };
    double x = expression(&p); space(&p);
    if (!p.message && *p.p) fail(&p, "unexpected character");
    if (error_pos) *error_pos = p.message ? (size_t)(p.error - p.start) : 0;
    if (!p.message) { *result = x; context->ans = x; context->has_ans = 1; }
    return p.message;
}
const char *calc_eval(const char *text, double *result, size_t *error_pos)
{
    calc_context context = {0};
    return calc_eval_context(text, &context, result, error_pos);
}
int calc_format(double value, char *out, size_t cap)
{
    return number_format(value, out, cap);
}
