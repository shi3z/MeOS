/*
 * Copyright (c) 2006, Adam Dunkels
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/* MeOS port: bounded parsing, bounded binary64 arithmetic, callback I/O, no OS exit. */
#include "ubasic.h"
#include "tokenizer.h"
#include <limits.h>
#include <float.h>
#include <m5os/number.h>
#include <m5os/number_math.h>
#include <setjmp.h>
#include <string.h>

#define MAX_LINES 128
#define MAX_DEPTH 16
static double variables[26];
static double array_cells[UBASIC_ARRAY_CELLS];
static unsigned array_size[26], array_offset[26], array_used;
static double *input_target;
static const char *input_prompt;
static size_t input_prompt_length;
static struct { int number; const char *text; } lines[MAX_LINES];
static int line_count, line_number, ended, depth, if_depth;
static const char *error_message, *following, *jump;
static const char *gosub[10];
static unsigned gosub_count, for_count;
static struct { const char *body; int var, floating; double to, step; } loops[4];
static ubasic_output output;
static void *output_context;
static jmp_buf recovery;
static ubasic_graphics graphics;
static int interrupted, degrees;

static void error(const char *message) { error_message = message; ended = 1; longjmp(recovery, 1); }
static int token(void) {
    if (tokenizer_token() == TOKENIZER_ERROR) error(tokenizer_error());
    return tokenizer_token();
}
static void next(void) { tokenizer_next(); (void)token(); }
static void accept(int expected) { if (token() != expected) error("unexpected token"); next(); }
static int end_of_line(void) { int t = token(); return t == TOKENIZER_CR || t == TOKENIZER_ENDOFINPUT; }
static int variable(void) {
    if (token() != TOKENIZER_VARIABLE) error("expected variable A-Z");
    int v = tokenizer_variable_num(); next(); return v;
}
static double checked(double n) { if (!number_valid(n)) error("numeric range exceeded"); return n; }
static int32_t integer(double n)
{
    int32_t result;
    if (!number_int32(n, &result)) error("integer argument required (signed 32-bit)");
    return result;
}
static double expression(void);
static double *reference(void)
{
    int v = variable();
    if (token() != TOKENIZER_LEFTPAREN) return &variables[v];
    next(); int32_t index = integer(expression()); accept(TOKENIZER_RIGHTPAREN);
    if (!array_size[v]) error("array is not DIMensioned");
    if (index < 0 || (uint32_t)index >= array_size[v]) error("array index out of range");
    return &array_cells[array_offset[v] + (unsigned)index];
}
static double factor(void)
{
    if (++depth > MAX_DEPTH) error("expression too deep");
    double r;
    int first = token();
    switch (first) {
    case TOKENIZER_PLUS: next(); r = factor(); break;
    case TOKENIZER_MINUS: next(); r = checked(-factor()); break;
    case TOKENIZER_NUMBER: r = tokenizer_num(); next(); break;
    case TOKENIZER_PI: next(); r = NUMBER_PI; break;
    case TOKENIZER_SIN: case TOKENIZER_COS: case TOKENIZER_TAN: case TOKENIZER_SQRT:
    case TOKENIZER_LOG: case TOKENIZER_LOG10: case TOKENIZER_EXP: case TOKENIZER_ABS:
    case TOKENIZER_INT: case TOKENIZER_FIX: {
        int op = first == TOKENIZER_SIN ? NUMBER_SIN : first == TOKENIZER_COS ? NUMBER_COS :
                 first == TOKENIZER_TAN ? NUMBER_TAN : first == TOKENIZER_SQRT ? NUMBER_SQRT :
                 first == TOKENIZER_LOG ? NUMBER_LOG : first == TOKENIZER_LOG10 ? NUMBER_LOG10 :
                 first == TOKENIZER_EXP ? NUMBER_EXP : first == TOKENIZER_ABS ? NUMBER_ABS :
                 first == TOKENIZER_INT ? NUMBER_INT : NUMBER_FIX;
        next(); accept(TOKENIZER_LEFTPAREN); double x = expression(); accept(TOKENIZER_RIGHTPAREN);
        const char *message = number_math(op, x, degrees, &r);
        if (message) error(message);
        break;
    }
    case TOKENIZER_VARIABLE: r = *reference(); break;
    case TOKENIZER_LEFTPAREN: next(); r = expression(); accept(TOKENIZER_RIGHTPAREN); break;
    default: error("expected number, variable or '('"); return 0;
    }
    if (first != TOKENIZER_PLUS && first != TOKENIZER_MINUS && token() == TOKENIZER_POWER) {
        next(); double exponent = factor();
        const char *message = number_power(r, exponent, &r);
        if (message) error(message);
    }
    depth--; return r;
}
static double term(void)
{
    double a = factor();
    while (token() == TOKENIZER_ASTR || token() == TOKENIZER_SLASH || token() == TOKENIZER_MOD) {
        int op = token(); next(); double b = factor();
        if (op == TOKENIZER_ASTR) a = checked(a * b);
        else if (op == TOKENIZER_SLASH) {
            if (!b) error("division by zero");
            a = checked(a / b);
        } else {
            int32_t ia = integer(a), ib = integer(b);
            if (!ib) error("division by zero");
            a = (double)((int64_t)ia % ib);
        }
    }
    return a;
}
static double expression(void)
{
    double a = term();
    while (token() == TOKENIZER_PLUS || token() == TOKENIZER_MINUS || token() == TOKENIZER_AND || token() == TOKENIZER_OR) {
        int op = token(); next(); double b = term();
        if (op == TOKENIZER_PLUS) a = checked(a + b);
        else if (op == TOKENIZER_MINUS) a = checked(a - b);
        else {
            int32_t ia = integer(a), ib = integer(b);
            a = op == TOKENIZER_AND ? ia & ib : ia | ib;
        }
    }
    return a;
}
static int relation(void)
{
    double a = expression(); int op = token();
    if (op != TOKENIZER_LT && op != TOKENIZER_GT && op != TOKENIZER_EQ) return a != 0;
    next(); int negate = 0, equal = 0;
    if (op == TOKENIZER_LT && token() == TOKENIZER_GT) { negate = 1; next(); }
    else if (op != TOKENIZER_EQ && token() == TOKENIZER_EQ) { equal = 1; next(); }
    double b = expression();
    if (negate) return a != b;
    return op == TOKENIZER_EQ ? a == b : op == TOKENIZER_LT ? (equal ? a <= b : a < b) : (equal ? a >= b : a > b);
}
static void write_text(const char *s) { if (output) output(s, strlen(s), output_context); }
static void write_number(double n)
{
    char buf[24]; int len = number_format(n, buf, sizeof buf);
    if (!len) error("cannot format number");
    if (output) output(buf, (size_t)len, output_context);
}
static const char *find_line(int n)
{
    for (int i = 0; i < line_count; i++) if (lines[i].number == n) return lines[i].text;
    error("no such line"); return NULL;
}
static int target(void) {
    int32_t n = integer(expression()); if (n < 1 || n > 65535) error("line number out of range"); return n;
}
static void skip_rest(void) { while (!end_of_line()) next(); }
static void skip_loop(int var)
{
    unsigned nesting = 1;
    tokenizer_goto(following);
    while (!tokenizer_finished()) {
        accept(TOKENIZER_NUMBER);
        if (token() == TOKENIZER_FOR) nesting++;
        if (token() == TOKENIZER_NEXT && --nesting == 0) {
            next(); if (variable() != var) error("NEXT variable mismatch");
            if (!end_of_line()) error("unexpected text after NEXT");
            if (token() == TOKENIZER_CR) next();
            jump = tokenizer_pos(); return;
        }
        skip_rest(); if (token() == TOKENIZER_CR) next();
    }
    error("FOR without NEXT");
}
static void graphics_statement(int op)
{
    int32_t args[5] = {0}; const char *str = NULL; size_t length = 0;
    next();
    if (op == UBASIC_TEXT) {
        args[0] = integer(expression()); accept(TOKENIZER_COMMA);
        args[1] = integer(expression()); accept(TOKENIZER_COMMA);
        if (token() != TOKENIZER_STRING) error("TEXT requires a literal string");
        str = tokenizer_pos()+1; length = tokenizer_string_length(); next();
        accept(TOKENIZER_COMMA); args[2] = integer(expression());
    } else {
        int count = op == UBASIC_LINE ? 5 : op == UBASIC_PSET ? 3 : 1;
        if (!(op == UBASIC_CLS && (end_of_line() || token() == TOKENIZER_ELSE))) {
            for (int i = 0; i < count; i++) {
                if (i) accept(TOKENIZER_COMMA);
                args[i] = integer(expression());
            }
        }
    }
    if (!end_of_line() && token() != TOKENIZER_ELSE) error("unexpected text after graphics statement");
    if (op == UBASIC_SCREEN && args[0] != 0 && args[0] != 1) error("SCREEN mode must be 0 or 1");
    if (op == UBASIC_WAIT && (args[0] < 0 || args[0] > 60000)) error("WAIT must be 0..60000 milliseconds");
    if (op == UBASIC_CLS || op == UBASIC_PSET || op == UBASIC_LINE || op == UBASIC_TEXT) {
        int color = op == UBASIC_CLS ? 0 : op == UBASIC_LINE ? 4 : 2;
        if (args[color] < 0 || args[color] > 65535) error("color must be RGB565 0..65535");
    }
    if (!graphics) error("graphics unavailable");
    const char *message = graphics(output_context, op, args, str, length);
    if (interrupted) error("interrupted");
    if (message) error(message);
}
static void statement(void)
{
    int t = token();
    switch (t) {
    case TOKENIZER_DEG: case TOKENIZER_RAD:
        next();
        if (!end_of_line() && token() != TOKENIZER_ELSE) error("unexpected text after angle mode");
        degrees = t == TOKENIZER_DEG; break;
    case TOKENIZER_SCREEN: graphics_statement(UBASIC_SCREEN); break;
    case TOKENIZER_CLS: graphics_statement(UBASIC_CLS); break;
    case TOKENIZER_PSET: graphics_statement(UBASIC_PSET); break;
    case TOKENIZER_LINE: graphics_statement(UBASIC_LINE); break;
    case TOKENIZER_TEXT: graphics_statement(UBASIC_TEXT); break;
    case TOKENIZER_WAIT: graphics_statement(UBASIC_WAIT); break;
    case TOKENIZER_PRINT: {
        next(); int newline = 1;
        while (!end_of_line() && token() != TOKENIZER_ELSE) {
            if (token() == TOKENIZER_STRING) {
                if (output) output(tokenizer_pos()+1, tokenizer_string_length(), output_context);
                next();
            } else write_number(expression());
            newline = 1;
            if (token() == TOKENIZER_COMMA || token() == TOKENIZER_SEMICOLON) {
                if (token() == TOKENIZER_COMMA) write_text(" ");
                next(); newline = 0;
            } else break;
        }
        if (newline) write_text("\n");
        break;
    }
    case TOKENIZER_LET: next(); /* fall through */
    case TOKENIZER_VARIABLE: {
        double *destination = reference(); accept(TOKENIZER_EQ); double n = expression();
        *destination = n; break;
    }
    case TOKENIZER_DIM: {
        next();
        for (;;) {
            int v = variable(); accept(TOKENIZER_LEFTPAREN);
            int32_t upper = integer(expression()); accept(TOKENIZER_RIGHTPAREN);
            if (array_size[v]) error("array already DIMensioned");
            if (upper < 0) error("negative array bound");
            if ((uint32_t)upper >= UBASIC_ARRAY_CELLS - array_used) error("array storage full (1024 elements)");
            unsigned count = (unsigned)upper + 1;
            array_offset[v] = array_used; array_size[v] = count;
            memset(array_cells + array_used, 0, count * sizeof array_cells[0]);
            array_used += count;
            if (token() != TOKENIZER_COMMA) break;
            next();
        }
        break;
    }
    case TOKENIZER_INPUT: {
        next(); input_prompt = NULL; input_prompt_length = 0;
        if (token() == TOKENIZER_STRING) {
            input_prompt = tokenizer_pos() + 1; input_prompt_length = tokenizer_string_length();
            next();
            if (token() != TOKENIZER_SEMICOLON && token() != TOKENIZER_COMMA) error("expected ';' after INPUT prompt");
            next();
        }
        input_target = reference();
        break;
    }
    case TOKENIZER_IF: {
        if (if_depth++) error("nested inline IF is not supported");
        next(); int condition = relation(); accept(TOKENIZER_THEN);
        if (condition) {
            statement();
            if (token() == TOKENIZER_ELSE) {
                while (!end_of_line()) {
                    if (token() == TOKENIZER_IF) error("nested inline IF is not supported");
                    next();
                }
            }
        } else {
            while (!end_of_line() && token() != TOKENIZER_ELSE) {
                if (token() == TOKENIZER_IF) error("nested inline IF is not supported");
                next();
            }
            if (token() == TOKENIZER_ELSE) { next(); statement(); }
        }
        if_depth--; break;
    }
    case TOKENIZER_GOTO: next(); jump = find_line(target()); break;
    case TOKENIZER_GOSUB:
        next(); if (gosub_count == 10) error("GOSUB stack full");
        jump = find_line(target()); gosub[gosub_count++] = following; break;
    case TOKENIZER_RETURN:
        next(); if (!gosub_count) error("RETURN without GOSUB");
        jump = gosub[--gosub_count]; break;
    case TOKENIZER_FOR: {
        if (if_depth) error("FOR must start a line");
        next(); int v = variable(); accept(TOKENIZER_EQ); double start = expression();
        accept(TOKENIZER_TO); double to = expression(), step = 1;
        int floating = token() == TOKENIZER_STEP;
        if (floating) {
            next(); step = expression();
            if (!step) error("STEP must be nonzero");
        } else { (void)integer(start); (void)integer(to); }
        if (!end_of_line()) error("unexpected text after FOR");
        variables[v] = start;
        if ((step > 0 && start > to) || (step < 0 && start < to)) { skip_loop(v); return; }
        if (for_count == 4) error("FOR stack full");
        loops[for_count].body = following; loops[for_count].var = v;
        loops[for_count].to = to; loops[for_count].step = step;
        loops[for_count++].floating = floating;
        break;
    }
    case TOKENIZER_NEXT: {
        next(); int v = variable();
        if (!for_count || loops[for_count-1].var != v) error("NEXT without matching FOR");
        unsigned frame = for_count-1;
        double value;
        if (!loops[frame].floating) {
            int32_t counter = integer(variables[v]);
            if (counter == INT32_MAX) error("FOR counter out of range");
            value = (double)counter + 1;
        } else {
            double current = variables[v], step = loops[frame].step, to = loops[frame].to;
            value = current + step;
            if (value == current) error("STEP does not advance the counter");
            /* Snap an almost-reached endpoint, never the following increment.
             * Limit tolerance by both rounding scale and a tiny step fraction;
             * this does not change ordinary expression comparisons. */
            double scale = current < 0 ? -current : current;
            double abs_to = to < 0 ? -to : to;
            double abs_step = step < 0 ? -step : step;
            if (abs_to > scale) scale = abs_to;
            if (abs_step > scale) scale = abs_step;
            double tolerance = 8 * DBL_EPSILON * scale;
            if (tolerance > abs_step * 1e-9) tolerance = abs_step * 1e-9;
            double delta = value - to; if (delta < 0) delta = -delta;
            if (current != to && delta <= tolerance) value = to;
            value = checked(value);
        }
        variables[v] = value;
        if ((loops[frame].step > 0 && value > loops[frame].to) ||
            (loops[frame].step < 0 && value < loops[frame].to)) for_count--;
        else jump = loops[frame].body;
        break;
    }
    case TOKENIZER_REM: next(); break;
    case TOKENIZER_END: next(); ended = 1; break;
    default: error("unsupported statement");
    }
}
void ubasic_init(const char *program, ubasic_output out, void *opaque, int clear_variables)
{
    output = out; output_context = opaque; graphics = NULL; interrupted = 0;
    ended = depth = if_depth = line_count = line_number = 0;
    error_message = NULL; gosub_count = for_count = 0;
    input_target = NULL; input_prompt = NULL; input_prompt_length = 0;
    if (clear_variables) {
        degrees = 0;
        memset(variables, 0, sizeof variables);
        memset(array_size, 0, sizeof array_size); array_used = 0;
        memset(array_cells, 0, sizeof array_cells);
    }
    /* Build a bounded complete index first; GOTO cannot hang searching for a missing line. */
    const char *p = program;
    int previous = 0;
    while (*p) {
        const char *start = p;
        unsigned n = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (*p++ - '0');
            if (n > 65535) break;
        }
        if (n <= (unsigned)previous || n > 65535 || (*p != ' ' && *p != '\t') || line_count == MAX_LINES) {
            error_message = "expected ascending unique line numbers 1..65535 (max 128 lines)"; ended = 1; break;
        }
        previous = n;
        lines[line_count].number = n; lines[line_count++].text = start;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    tokenizer_init(program);
}
void ubasic_run(void)
{
    if (ended || input_target || tokenizer_finished()) return;
    if (setjmp(recovery)) { input_target = NULL; return; }
    depth = if_depth = 0; jump = NULL;
    line_number = token() == TOKENIZER_NUMBER ? integer(tokenizer_num()) : 0;
    following = tokenizer_pos();
    while (*following && *following != '\n') following++;
    if (*following) following++;
    accept(TOKENIZER_NUMBER);
    statement();
    /* skip_loop already advanced the tokenizer to the next line. */
    if (jump && token() == TOKENIZER_NUMBER && tokenizer_pos() == jump) return;
    if (!end_of_line()) error("unexpected text after statement");
    if (jump) tokenizer_goto(jump);
    else if (token() == TOKENIZER_CR) next();
    if (!ended && tokenizer_finished() && for_count) error("FOR without NEXT");
    if (input_target) {
        if (input_prompt && output) output(input_prompt, input_prompt_length, output_context);
        write_text("? ");
    }
}
int ubasic_finished(void) { return ended || (!input_target && tokenizer_finished()); }
const char *ubasic_error(void) { return error_message; }
int ubasic_error_line(void) { return line_number; }
double ubasic_get_variable(int index) { return index >= 0 && index < 26 ? variables[index] : 0; }

int ubasic_waiting_input(void) { return !ended && input_target != NULL; }
void ubasic_stop(void) { ended = 1; input_target = NULL; }
const char *ubasic_supply_input(const char *text)
{
    if (!ubasic_waiting_input()) return "not waiting for INPUT";
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    double value;
    const char *end;
    const char *message = number_parse(p, &end, &value);
    if (message) return message;
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return "expected decimal number";
    *input_target = value;
    input_target = NULL;
    return NULL;
}

void ubasic_set_graphics(ubasic_graphics callback) { graphics = callback; }
void ubasic_interrupt(void) { interrupted = 1; }
int ubasic_interrupted(void) { return interrupted; }
