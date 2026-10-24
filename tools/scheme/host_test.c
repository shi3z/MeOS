#include "../../user/scheme/buffer.h"
#include "../../user/scheme/scheme.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char output[2048];
static unsigned plots;
static bool interrupt;
static void write_text(void *ctx, const char *s)
{
    (void)ctx;
    if (strlen(output) + strlen(s) < sizeof output)
        strcat(output, s);
}
static void plot(void *ctx, const double *x, const double *y, unsigned n, const char *title)
{
    (void)ctx;
    assert(title && title[0]);
    assert(n == 129);
    assert(x[0] == 0 && x[n - 1] == 30);
    for (unsigned i = 0; i < n; i++)
        assert(isfinite(y[i]));
    plots++;
}
static bool cancel(void *ctx)
{
    (void)ctx;
    return interrupt;
}
static char result[512];
static void expect(scheme *s, const char *source, const char *want)
{
    bool ok = scheme_eval(s, source, false, result, sizeof result);
    if (!ok || strcmp(result, want)) {
        fprintf(stderr, "%s -> %s (expected %s)\n", source, result, want);
        abort();
    }
}
static void bad(scheme *s, const char *source)
{
    assert(!scheme_eval(s, source, false, result, sizeof result));
    assert(result[0]);
}
static void previous(scheme_buffer *b, const char *source, size_t point, const char *want)
{
    assert(scheme_buffer_set(b, source, strlen(source)));
    size_t begin = 0, end = 0;
    bool ok = scheme_buffer_previous_form(b, point, &begin, &end);
    if (!want) {
        assert(!ok);
        return;
    }
    assert(ok && end <= point && begin < end);
    assert(end - begin == strlen(want) && !memcmp(source + begin, want, end - begin));
}
int main(void)
{
    void *workspace = malloc(scheme_workspace_size());
    assert(workspace);
    scheme *s =
        scheme_init(workspace, (scheme_host){.write = write_text, .plot = plot, .cancel = cancel});
    expect(s, "(+ 2 (* 3 4))", "14");
    expect(s, "(/ 4)", "0.25");
    expect(s, "(if #f 1 2)", "2");
    expect(s, "(if 0 1 2)", "1");
    expect(s, "(let ((x 3) (y 4)) (+ x y))", "7");
    expect(s, "(and #t 3)", "3");
    expect(s, "(or #f 7)", "7");
    expect(s, "(and #f (/ 1 0))", "#f");
    expect(s, "(or 8 (/ 1 0))", "8");
    expect(s, "'()", "()");
    expect(s, "'(a b)", "(a b)");
    expect(s, "(car (cons 1 2))", "1");
    expect(s, "(cdr (cons 1 2))", "2");
    expect(s, "(define (make x) (lambda (y) (+ x y))) (define add7 (make 7)) (add7 8)", "15");
    expect(s, "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 6)", "720");
    expect(s, "(define (振動 x) (* (sin x) (exp (* -0.1 x)))) (plot 振動 0 30)", "()");
    assert(plots == 1);
    expect(s, "(display \"日本語\") (newline)", "()");
    assert(!strcmp(output, "日本語\n"));
    expect(s, "(define x 4)", "x");
    assert(scheme_eval(s, "(set! x 99) (+ 2 3)", true, result, sizeof result));
    assert(!strcmp(result, "5"));
    expect(s, "x", "4");
    bad(s, "(set! x 88) (");
    expect(s, "x", "4"); /* Parse before effects. */
    expect(s, "(define x 5)", "x");
    expect(s, "x", "5");
    for (unsigned i = 0; i < 3000; i++)
        expect(s, "(add7 3)", "10"); /* collect keeps closure environments */
    bad(s, "(/ 1 0)");
    bad(s, "(sqrt -1)");
    bad(s, "(log 0)");
    bad(s, "(exp 1000)");
    bad(s, "1e999");
    bad(s, "(1 2)");
    bad(s, "(car '())");
    bad(s, "(lambda (x x) x)");
    bad(s, "(let ((x 1) (x 2)) x)");
    bad(s, "(* 1e100 1e100)");
    bad(s, "(insert-text! 1)");
    bad(s, "(insert-text! \"host callback unavailable\")");
    bad(s, "\xc0\xaf");
    bad(s, "(define (loop x) (loop x)) (loop 1)");
    expect(s, "(+ 1 2)", "3");
    interrupt = true;
    bad(s, "(plot 振動 0 30)");
    interrupt = false;
    assert(plots == 1);
    /* Plot failure must not publish partial samples; VM recovers afterwards. */
    bad(s, "(plot (lambda (x) (/ 1 x)) 0 30)");
    assert(plots == 1);
    expect(s, "(plot 振動 0 30)", "()");
    assert(plots == 2);
    /* A function can retain a sample value; later samples must not mutate it. */
    expect(s,
           "(define kept #f) (plot (lambda (x) (if kept 0 (begin (set! kept x) "
           "0))) 0 30) kept",
           "0");
    scheme_buffer *b = calloc(1, sizeof *b);
    assert(b);
    assert(scheme_buffer_set(b, "(日本語)", 11));
    b->cursor = 10;
    assert(scheme_buffer_delete(b, true));
    assert(!strcmp(b->text, "(日本)"));
    scheme_buffer_undo(b);
    assert(!strcmp(b->text, "(日本語)"));
    b->cursor = 1;
    assert(scheme_buffer_insert(b, "変換", 6));
    scheme_buffer_undo(b);
    assert(!strcmp(b->text, "(日本語)"));
    assert(!scheme_buffer_insert(b, "\xc0\xaf", 2));
    assert(!scheme_buffer_set(b, "\xed\xa0\x80", 3));
    assert(!scheme_buffer_set(b, "\xf4\x90\x80\x80", 4));
    assert(!scheme_buffer_set(b, "a\0b", 3));
    assert(scheme_buffer_set(b, "\xf0\x9f\x98\x80", 4));
    assert(scheme_buffer_next(b, 0) == 4);
    assert(scheme_buffer_prev(b, 4) == 0);
    previous(b, "(begin (+ 1 2)", strlen("(begin (+ 1 2)"), "(+ 1 2)");
    previous(b, "(begin 42", strlen("(begin 42"), "42");
    previous(b, "(begin ' (a b)", strlen("(begin ' (a b)"), "' (a b)");
    previous(b, "''日本語", strlen("''日本語"), "''日本語");
    previous(b, "(x \"括弧 ); と \\\"\"", strlen("(x \"括弧 ); と \\\"\""), "\"括弧 ); と \\\"\"");
    previous(b, "(+ 1 2) ; (ignored)\n  ", strlen("(+ 1 2) ; (ignored)\n  "), "(+ 1 2)");
    previous(b, "(+ 1 2) (", strlen("(+ 1 2) ("), NULL);
    previous(b, "(+ 1 2) '\n", strlen("(+ 1 2) '\n"), NULL);
    previous(b, "(+ 1 2) \"unfinished", strlen("(+ 1 2) \"unfinished"), NULL);
    previous(b, "(+ 1 2) \"escape\\", strlen("(+ 1 2) \"escape\\"), NULL);
    previous(b, ") (+ 1 2)", strlen(") (+ 1 2)"), NULL);
    previous(b, "() (bad tail", 2, "()");
    previous(b, "日本語", 1, NULL);
    previous(b, "", 0, NULL);
    /* Deterministic malformed-reader campaign; no state accumulates on errors. */
    unsigned rng = 7;
    char fuzz[160];
    const char alphabet[] = "()'\";\\ abc123#\n";
    for (unsigned run = 0; run < 2500; run++) {
        s = scheme_init(workspace, (scheme_host){0});
        unsigned len = run % sizeof fuzz;
        for (unsigned j = 0; j < len; j++) {
            rng = rng * 1664525u + 1013904223u;
            fuzz[j] = alphabet[rng % (sizeof alphabet - 1)];
        }
        fuzz[len] = 0;
        scheme_eval(s, fuzz, false, result, sizeof result);
        assert(scheme_buffer_set(b, fuzz, len));
        for (size_t point = 0; point <= len; point++) {
            size_t begin, end;
            if (scheme_buffer_previous_form(b, point, &begin, &end))
                assert(begin < end && end <= point);
        }
    }
    free(b);
    free(workspace);
    puts("Scheme: closures, GC, plotting, limits, UTF-8 editing, malformed "
         "reader PASS");
    return 0;
}
