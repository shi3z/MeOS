#include "scheme.h"
#include "buffer.h"
#include <m5os/number.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define CELLS 4096
#define TEXT 128
#define DEPTH 64
#define BUDGET 60000
#define SAMPLES 129
typedef uint16_t ref;
enum { FREE, NIL, NUMBER, SYMBOL, STRING, PAIR, CLOSURE, NATIVE, BOOLEAN };
typedef struct {
    uint8_t type, mark;
    ref a, b, env;
    double number;
    char text[TEXT];
} cell;
struct scheme {
    cell cells[CELLS];
    ref global, yes, no, next;
    unsigned steps;
    bool plotting;
    scheme_host host;
    const char *source, *cursor, *error;
    size_t error_at;
    ref queue[CELLS];
    double xs[SAMPLES], ys[SAMPLES];
};
static ref eval(scheme *, ref, ref, unsigned);
static ref apply(scheme *, ref, ref, unsigned);
static ref fail(scheme *s, const char *e)
{
    if (!s->error) {
        s->error = e;
        s->error_at = (size_t)(s->cursor - s->source);
    }
    return 0;
}
static bool tick(scheme *s, unsigned d)
{
    if (s->error)
        return false;
    if (d >= DEPTH) {
        fail(s, "evaluation nesting limit");
        return false;
    }
    if (++s->steps > BUDGET) {
        fail(s, "evaluation budget exceeded");
        return false;
    }
    if ((s->steps & 255u) == 0 && s->host.cancel && s->host.cancel(s->host.context)) {
        fail(s, "interrupted");
        return false;
    }
    return true;
}
static ref alloc(scheme *s, int type)
{
    for (unsigned i = 1; i < CELLS; i++) {
        ref r = s->next++;
        if (s->next == CELLS)
            s->next = 1;
        if (s->cells[r].type == FREE) {
            memset(&s->cells[r], 0, sizeof(cell));
            s->cells[r].type = (uint8_t)type;
            return r;
        }
    }
    return fail(s, "cell heap exhausted; reset environment or shorten evaluation");
}
static ref pair(scheme *s, ref a, ref b)
{
    ref r = alloc(s, PAIR);
    if (r) {
        s->cells[r].a = a;
        s->cells[r].b = b;
    }
    return r;
}
static ref num(scheme *s, double n)
{
    if (!number_valid(n))
        return fail(s, "numeric range exceeded (zero or magnitude 1e-100..1e100)");
    ref r = alloc(s, NUMBER);
    if (r)
        s->cells[r].number = n;
    return r;
}
static bool named(scheme *s, ref r, const char *name)
{
    return s->cells[r].type == SYMBOL && !strcmp(s->cells[r].text, name);
}
static bool proper(scheme *s, ref r)
{
    unsigned n = 0;
    while (r) {
        if (s->cells[r].type != PAIR || ++n >= CELLS)
            return false;
        r = s->cells[r].b;
    }
    return true;
}
static int count(scheme *s, ref r)
{
    int n = 0;
    while (r) {
        if (s->cells[r].type != PAIR)
            return -1;
        n++;
        r = s->cells[r].b;
    }
    return n;
}
static ref at(scheme *s, ref r, int n)
{
    while (r && n--)
        r = s->cells[r].b;
    return r ? s->cells[r].a : 0;
}
static ref binding(scheme *s, ref env, ref sym)
{
    for (ref e = env; e; e = s->cells[e].b)
        for (ref p = s->cells[e].a; p; p = s->cells[p].b) {
            ref b = s->cells[p].a;
            if (!strcmp(s->cells[s->cells[b].a].text, s->cells[sym].text))
                return b;
        }
    return 0;
}
static void define(scheme *s, ref env, ref name, ref value)
{
    if (s->cells[name].type != SYMBOL) {
        fail(s, "expected identifier");
        return;
    }
    for (ref p = s->cells[env].a; p; p = s->cells[p].b) {
        ref b = s->cells[p].a;
        if (!strcmp(s->cells[s->cells[b].a].text, s->cells[name].text)) {
            s->cells[b].b = value;
            return;
        }
    }
    ref b = pair(s, name, value), p = pair(s, b, s->cells[env].a);
    if (!s->error)
        s->cells[env].a = p;
}
/* Mark/sweep only between evaluations: parser/evaluator temporaries remain
 * stable during recursion. Cyclic closure/environment graphs are supported. */
static void collect(scheme *s)
{
    unsigned head = 0, tail = 0;
    ref roots[] = {s->global, s->yes, s->no};
    for (unsigned i = 0; i < 3; i++)
        if (roots[i] && !s->cells[roots[i]].mark) {
            s->cells[roots[i]].mark = 1;
            s->queue[tail++] = roots[i];
        }
    while (head < tail) {
        cell *c = &s->cells[s->queue[head++]];
        if (c->type != PAIR && c->type != CLOSURE)
            continue;
        ref edges[] = {c->a, c->b, c->env};
        for (unsigned i = 0; i < 3; i++)
            if (edges[i] && !s->cells[edges[i]].mark) {
                s->cells[edges[i]].mark = 1;
                s->queue[tail++] = edges[i];
            }
    }
    for (unsigned i = 1; i < CELLS; i++) {
        if (!s->cells[i].mark)
            s->cells[i].type = FREE;
        s->cells[i].mark = 0;
    }
}
static void space(scheme *s)
{
    for (;;) {
        while (*s->cursor == ' ' || *s->cursor == '\n' || *s->cursor == '\r' || *s->cursor == '\t')
            s->cursor++;
        if (*s->cursor != ';')
            return;
        while (*s->cursor && *s->cursor != '\n')
            s->cursor++;
    }
}
static bool delimiter(char c)
{
    return !c || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' ||
           c == ';';
}
static ref read_form(scheme *s, unsigned depth)
{
    if (depth >= DEPTH)
        return fail(s, "reader nesting limit");
    space(s);
    char ch = *s->cursor++;
    if (!ch) {
        s->cursor--;
        return fail(s, "expected expression");
    }
    if (ch == '(') {
        ref first = 0, tail = 0;
        space(s);
        while (*s->cursor != ')') {
            if (!*s->cursor)
                return fail(s, "unclosed list");
            ref v = read_form(s, depth + 1);
            if (s->error)
                return 0;
            ref p = pair(s, v, 0);
            if (s->error)
                return 0;
            if (tail)
                s->cells[tail].b = p;
            else
                first = p;
            tail = p;
            space(s);
        }
        s->cursor++;
        return first;
    }
    if (ch == ')')
        return fail(s, "unexpected closing parenthesis");
    if (ch == '\'') {
        ref q = alloc(s, SYMBOL);
        if (q)
            strcpy(s->cells[q].text, "quote");
        ref v = read_form(s, depth + 1);
        return pair(s, q, pair(s, v, 0));
    }
    char text[TEXT];
    unsigned n = 0;
    if (ch == '"') {
        while (*s->cursor && *s->cursor != '"') {
            char c = *s->cursor++;
            if (c == '\\') {
                c = *s->cursor;
                if (!c)
                    return fail(s, "unclosed string escape");
                s->cursor++;
                if (c == 'n')
                    c = '\n';
                else if (c == 't')
                    c = '\t';
                else if (c != '"' && c != '\\')
                    return fail(s, "unsupported string escape");
            }
            if (n == TEXT - 1)
                return fail(s, "string exceeds 127 bytes");
            text[n++] = c;
        }
        if (!*s->cursor)
            return fail(s, "unclosed string");
        s->cursor++;
        text[n] = 0;
        ref r = alloc(s, STRING);
        if (r)
            strcpy(s->cells[r].text, text);
        return r;
    }
    s->cursor--;
    while (!delimiter(*s->cursor)) {
        if ((unsigned char)*s->cursor < 32 || *s->cursor == '"')
            return fail(s, "invalid identifier character");
        if (n == TEXT - 1)
            return fail(s, "token exceeds 127 bytes");
        text[n++] = *s->cursor++;
    }
    text[n] = 0;
    if (!strcmp(text, "#t"))
        return s->yes;
    if (!strcmp(text, "#f"))
        return s->no;
    /* Locale-free bounded decimal reader; no libc strtod/errno/locks. */
    unsigned i = 0;
    double sign = 1, v = 0, scale = 1;
    if (text[i] == '+' || text[i] == '-') {
        if (text[i++] == '-')
            sign = -1;
    }
    bool digits = false;
    while (text[i] >= '0' && text[i] <= '9') {
        digits = true;
        v = v * 10 + (text[i++] - '0');
    }
    if (text[i] == '.') {
        i++;
        while (text[i] >= '0' && text[i] <= '9') {
            digits = true;
            scale *= .1;
            v += (text[i++] - '0') * scale;
        }
    }
    if (digits && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        int es = 1, e = 0;
        if (text[i] == '+' || text[i] == '-') {
            if (text[i++] == '-')
                es = -1;
        }
        if (text[i] < '0' || text[i] > '9')
            return fail(s, "invalid decimal exponent");
        while (text[i] >= '0' && text[i] <= '9') {
            e = e * 10 + (text[i++] - '0');
            if (e > 100)
                return fail(s, "decimal exponent exceeds 100");
        }
        while (e--)
            v = es > 0 ? v * 10 : v / 10;
    }
    if (digits && !text[i])
        return num(s, sign * v);
    if (!n || (digits && text[i]))
        return fail(s, "invalid numeric literal");
    if (text[0] == '#' || !strcmp(text, "."))
        return fail(s, "unsupported reader syntax");
    ref r = alloc(s, SYMBOL);
    if (r)
        strcpy(s->cells[r].text, text);
    return r;
}
static ref sequence(scheme *s, ref body, ref env, unsigned d)
{
    ref v = 0;
    while (body && !s->error) {
        v = eval(s, s->cells[body].a, env, d + 1);
        body = s->cells[body].b;
    }
    return v;
}
static bool parameters(scheme *s, ref p)
{
    if (!proper(s, p))
        return false;
    for (ref a = p; a; a = s->cells[a].b) {
        ref sym = s->cells[a].a;
        if (s->cells[sym].type != SYMBOL)
            return false;
        for (ref b = s->cells[a].b; b; b = s->cells[b].b)
            if (!strcmp(s->cells[sym].text, s->cells[s->cells[b].a].text))
                return false;
    }
    return true;
}
static ref closure(scheme *s, ref params, ref body, ref env)
{
    if (!parameters(s, params) || !body)
        return fail(s, "lambda needs distinct identifiers and a body");
    ref f = alloc(s, CLOSURE);
    if (f) {
        s->cells[f].a = params;
        s->cells[f].b = body;
        s->cells[f].env = env;
    }
    return f;
}
static ref eval(scheme *s, ref r, ref env, unsigned d)
{
    if (!tick(s, d))
        return 0;
    cell *c = &s->cells[r];
    if (c->type == SYMBOL) {
        ref b = binding(s, env, r);
        return b ? s->cells[b].b : fail(s, "unbound identifier");
    }
    if (c->type != PAIR)
        return r;
    ref op = c->a, args = c->b;
    int n = count(s, args);
    if (n < 0)
        return fail(s, "improper application");
    if (named(s, op, "quote")) {
        if (n != 1)
            return fail(s, "quote expects one argument");
        return at(s, args, 0);
    }
    if (named(s, op, "if")) {
        if (n != 2 && n != 3)
            return fail(s, "if expects two or three arguments");
        ref v = eval(s, at(s, args, 0), env, d + 1);
        return s->error ? 0 : eval(s, at(s, args, v == s->no ? 2 : 1), env, d + 1);
    }
    if (named(s, op, "begin"))
        return sequence(s, args, env, d);
    if (named(s, op, "and") || named(s, op, "or")) {
        bool is_and = named(s, op, "and");
        ref v = is_and ? s->yes : s->no;
        for (ref p = args; p && !s->error; p = s->cells[p].b) {
            v = eval(s, s->cells[p].a, env, d + 1);
            if ((v == s->no) == is_and)
                break;
        }
        return v;
    }
    if (named(s, op, "let")) {
        ref bindings = at(s, args, 0);
        if (n < 2 || !proper(s, bindings))
            return fail(s, "let needs bindings and body");
        ref local = pair(s, 0, env);
        for (ref p = bindings; p && !s->error; p = s->cells[p].b) {
            ref entry = s->cells[p].a, name = at(s, entry, 0);
            if (count(s, entry) != 2 || s->cells[name].type != SYMBOL)
                return fail(s, "let binding must be (identifier expression)");
            for (ref q = s->cells[local].a; q; q = s->cells[q].b) {
                ref previous = s->cells[s->cells[q].a].a;
                if (!strcmp(s->cells[previous].text, s->cells[name].text))
                    return fail(s, "duplicate let identifier");
            }
            ref v = eval(s, at(s, entry, 1), env, d + 1);
            if (!s->error)
                define(s, local, name, v);
        }
        return s->error ? 0 : sequence(s, s->cells[args].b, local, d);
    }
    if (named(s, op, "lambda")) {
        if (n < 2)
            return fail(s, "lambda expects parameters and body");
        return closure(s, at(s, args, 0), s->cells[args].b, env);
    }
    if (named(s, op, "define")) {
        if (n < 2)
            return fail(s, "define expects name and value");
        ref name = at(s, args, 0), value;
        if (s->cells[name].type == PAIR) {
            value = closure(s, s->cells[name].b, s->cells[args].b, env);
            name = s->cells[name].a;
        } else {
            if (n != 2)
                return fail(s, "define expects one value");
            value = eval(s, at(s, args, 1), env, d + 1);
        }
        if (!s->error)
            define(s, env, name, value);
        return name;
    }
    if (named(s, op, "set!")) {
        if (n != 2 || s->cells[at(s, args, 0)].type != SYMBOL)
            return fail(s, "set! expects identifier and value");
        ref b = binding(s, env, at(s, args, 0));
        if (!b)
            return fail(s, "set! of unbound identifier");
        ref v = eval(s, at(s, args, 1), env, d + 1);
        if (!s->error)
            s->cells[b].b = v;
        return v;
    }
    ref fn = eval(s, op, env, d + 1), values = 0, tail = 0;
    for (ref p = args; p && !s->error; p = s->cells[p].b) {
        ref v = eval(s, s->cells[p].a, env, d + 1), q = pair(s, v, 0);
        if (tail)
            s->cells[tail].b = q;
        else
            values = q;
        tail = q;
    }
    return s->error ? 0 : apply(s, fn, values, d + 1);
}
typedef struct {
    char *out;
    size_t cap, len;
} writer;
static void put(writer *w, const char *t)
{
    while (*t) {
        if (w->len + 1 < w->cap)
            w->out[w->len++] = *t;
        t++;
    }
    if (w->cap)
        w->out[w->len] = 0;
}
static void print(scheme *s, ref r, writer *w, unsigned depth, bool display)
{
    if (depth > 16) {
        put(w, "...");
        return;
    }
    cell *c = &s->cells[r];
    char b[32];
    switch (c->type) {
    case NIL:
        put(w, "()");
        break;
    case NUMBER:
        if (number_format(c->number, b, sizeof b))
            put(w, b);
        else
            put(w, "#<number outside display range>");
        break;
    case SYMBOL:
        put(w, c->text);
        break;
    case STRING:
        if (!display)
            put(w, "\"");
        put(w, c->text);
        if (!display)
            put(w, "\"");
        break;
    case BOOLEAN:
        put(w, r == s->no ? "#f" : "#t");
        break;
    case PAIR:
        put(w, "(");
        for (unsigned i = 0; r && i < 64; i++) {
            if (s->cells[r].type != PAIR) {
                put(w, " . ");
                print(s, r, w, depth + 1, display);
                r = 0;
                break;
            }
            if (i)
                put(w, " ");
            print(s, s->cells[r].a, w, depth + 1, display);
            r = s->cells[r].b;
        }
        if (r)
            put(w, " ...");
        put(w, ")");
        break;
    default:
        put(w, "#<procedure>");
        break;
    }
}
static const char *const natives[] = {
    "+",    "-",     "*",     "/",    "=",       "<",       ">",    "<=",           ">=",
    "sin",  "cos",   "exp",   "sqrt", "log",     "abs",     "cons", "car",          "cdr",
    "list", "null?", "pair?", "not",  "display", "newline", "plot", "insert-text!", NULL};
static ref apply(scheme *s, ref fn, ref args, unsigned d)
{
    if (!tick(s, d))
        return 0;
    cell *f = &s->cells[fn];
    int n = count(s, args);
    if (f->type == CLOSURE) {
        if (n != count(s, f->a))
            return fail(s, "procedure argument count mismatch");
        ref env = pair(s, 0, f->env);
        for (ref p = f->a, a = args; p && !s->error; p = s->cells[p].b, a = s->cells[a].b)
            define(s, env, s->cells[p].a, s->cells[a].a);
        return s->error ? 0 : sequence(s, f->b, env, d);
    }
    if (f->type != NATIVE)
        return fail(s, "attempt to call non-procedure");
    int op = (int)f->number;
    ref a = at(s, args, 0), b = at(s, args, 1);
    if (op == 18)
        return args;
    if (op == 15) {
        if (n != 2)
            return fail(s, "cons expects two arguments");
        return pair(s, a, b);
    }
    if (op == 16 || op == 17) {
        if (n != 1 || s->cells[a].type != PAIR)
            return fail(s, "car/cdr expects a pair");
        return op == 16 ? s->cells[a].a : s->cells[a].b;
    }
    if (op >= 19 && op <= 21) {
        if (n != 1)
            return fail(s, "predicate expects one argument");
        bool yes = op == 19 ? !a : op == 20 ? s->cells[a].type == PAIR : a == s->no;
        return yes ? s->yes : s->no;
    }
    if (op == 22 || op == 23) {
        if (n != (op == 22 ? 1 : 0))
            return fail(s, "display/newline argument count");
        char out[512] = {0};
        writer w = {out, sizeof out, 0};
        if (op == 22)
            print(s, a, &w, 0, true);
        else
            put(&w, "\n");
        if (s->host.write)
            s->host.write(s->host.context, out);
        return 0;
    }
    if (op == 25) {
        if (n != 1 || s->cells[a].type != STRING)
            return fail(s, "insert-text! expects a string");
        if (!s->host.insert_text || !s->host.insert_text(s->host.context, s->cells[a].text))
            return fail(s, "editor insertion unavailable or buffer full");
        return 0;
    }
    if (op == 24) {
        if ((n != 3 && n != 4) || s->cells[b].type != NUMBER ||
            s->cells[at(s, args, 2)].type != NUMBER ||
            (n == 4 && s->cells[at(s, args, 3)].type != STRING))
            return fail(s, "plot expects procedure, xmin, xmax, optional title string");
        if (!s->host.plot)
            return fail(s, "plot unavailable");
        if (s->plotting)
            return fail(s, "nested plot is unsupported");
        double lo = s->cells[b].number, hi = s->cells[at(s, args, 2)].number;
        if (!(hi > lo) || !isfinite(hi - lo))
            return fail(s, "invalid plot range");
        /* Each sample gets distinct values: callbacks can retain them in
         * closures. Entire plot shares the normal heap and execution budget. */
        s->plotting = true;
        for (unsigned i = 0; i < SAMPLES && !s->error; i++) {
            double v = lo + (hi - lo) * (double)i / (SAMPLES - 1);
            ref x = num(s, v), arg = pair(s, x, 0);
            if (s->error)
                return 0;
            ref y = apply(s, a, arg, d + 1);
            if (s->error)
                return 0;
            if (s->cells[y].type != NUMBER)
                return fail(s, "plot procedure must return a number");
            s->xs[i] = v;
            s->ys[i] = s->cells[y].number;
        }
        s->plotting = false;
        if (!s->error)
            s->host.plot(s->host.context, s->xs, s->ys, SAMPLES,
                         n == 4 ? s->cells[at(s, args, 3)].text : "関数グラフ");
        return 0;
    }
    if (op >= 9 && op <= 14) {
        if (n != 1 || s->cells[a].type != NUMBER)
            return fail(s, "math function expects one number");
        double x = s->cells[a].number;
        if ((op == 12 && x < 0) || (op == 13 && x <= 0) || (op == 11 && (x > 230 || x < -230)))
            return fail(s, "math domain/range error");
        return num(s, op == 9    ? sin(x)
                      : op == 10 ? cos(x)
                      : op == 11 ? exp(x)
                      : op == 12 ? sqrt(x)
                      : op == 13 ? log(x)
                                 : fabs(x));
    }
    if (op >= 4 && op <= 8) {
        if (n != 2 || s->cells[a].type != NUMBER || s->cells[b].type != NUMBER)
            return fail(s, "comparison expects two numbers");
        double x = s->cells[a].number, y = s->cells[b].number;
        bool yes = op == 4 ? x == y : op == 5 ? x < y : op == 6 ? x > y : op == 7 ? x <= y : x >= y;
        return yes ? s->yes : s->no;
    }
    if ((op == 1 || op == 3) && n == 0)
        return fail(s, "arithmetic needs an argument");
    double v = op == 2 ? 1 : 0;
    int i = 0;
    for (ref p = args; p; p = s->cells[p].b, i++) {
        ref r = s->cells[p].a;
        if (s->cells[r].type != NUMBER)
            return fail(s, "arithmetic expects numbers");
        double x = s->cells[r].number;
        if (op == 0)
            v += x;
        else if (op == 2)
            v *= x;
        else if (i == 0)
            v = x;
        else if (op == 1)
            v -= x;
        else {
            if (x == 0)
                return fail(s, "division by zero");
            v /= x;
        }
        if (!isfinite(v))
            return fail(s, "numeric overflow");
    }
    if (n == 1 && op == 1)
        v = -v;
    if (n == 1 && op == 3) {
        if (v == 0)
            return fail(s, "division by zero");
        v = 1 / v;
    }
    return num(s, v);
}
size_t scheme_workspace_size(void) { return sizeof(scheme); }
scheme *scheme_init(void *workspace, scheme_host host)
{
    scheme *s = workspace;
    memset(s, 0, sizeof *s);
    s->host = host;
    s->next = 1;
    s->cells[0].type = NIL;
    s->source = s->cursor = "";
    s->yes = alloc(s, BOOLEAN);
    s->no = alloc(s, BOOLEAN);
    s->global = pair(s, 0, 0);
    for (int i = 0; natives[i]; i++) {
        ref name = alloc(s, SYMBOL), fn = alloc(s, NATIVE);
        strcpy(s->cells[name].text, natives[i]);
        s->cells[fn].number = i;
        define(s, s->global, name, fn);
    }
    ref pi = alloc(s, SYMBOL);
    strcpy(s->cells[pi].text, "pi");
    define(s, s->global, pi, num(s, 3.14159265358979323846));
    return s;
}
bool scheme_eval(scheme *s, const char *source, bool last, char *out, size_t cap)
{
    s->source = s->cursor = source;
    s->error = NULL;
    s->error_at = 0;
    s->steps = 0;
    s->plotting = false;
    collect(s);
    if (strlen(source) > SCHEME_SOURCE_CAP || !scheme_utf8_text(source, strlen(source)))
        fail(s, "source exceeds 16 KiB or is invalid UTF-8 text");
    ref forms = 0, tail = 0, final = 0;
    space(s);
    while (*s->cursor && !s->error) {
        ref f = read_form(s, 0), p = pair(s, f, 0);
        if (tail)
            s->cells[tail].b = p;
        else
            forms = p;
        tail = p;
        final = f;
        space(s);
    }
    ref result = 0;
    if (!s->error)
        result = last ? eval(s, final, s->global, 0) : sequence(s, forms, s->global, 0);
    writer w = {out, cap, 0};
    if (cap)
        out[0] = 0;
    if (s->error)
        put(&w, s->error);
    else
        print(s, result, &w, 0, false);
    return !s->error;
}
size_t scheme_error_offset(const scheme *s) { return s->error_at; }
void scheme_visit_commands(const scheme *s, void (*visit)(void *, const char *), void *context)
{
    for (ref p = s->cells[s->global].a; p; p = s->cells[p].b) {
        ref binding = s->cells[p].a;
        const cell *value = &s->cells[s->cells[binding].b];
        if (value->type == CLOSURE && !value->a)
            visit(context, s->cells[s->cells[binding].a].text);
    }
}
