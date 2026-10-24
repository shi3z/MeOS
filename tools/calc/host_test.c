#include "../../apps/calculator/expr.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static void good(const char *s, double want, const char *display) {
    double x = -42; size_t at; char buf[24];
    const char *err = calc_eval(s, &x, &at);
    if (err) fprintf(stderr, "%s: %s at %zu\n", s, err, at);
    assert(!err); assert(fabs(x-want) <= fabs(want)*1e-12 + 1e-110);
    assert(calc_format(x, buf, sizeof buf));
    if (strcmp(buf, display)) fprintf(stderr, "%s: got %s wanted %s\n", s, buf, display);
    assert(!strcmp(buf, display));
}
int main(void) {
    good("2+3*4",14,"14"); good("(2+3)*4",20,"20");
    good("-2^2",-4,"-4"); good("2^3^2",512,"512");
    good("2^-3",.125,"0.125"); good(".1+.2",.3,"0.3");
    good("1.25e-3 * 2",.0025,"0.0025"); good("1e100",1e100,"1e+100");
    good("1e-100",1e-100,"1e-100"); good("-0",0,"0");
    good("999999999999+1",1e12,"1e+12"); good("1/3",1.0/3,"0.333333333333");
    good("sqrt(2)^2",2,"2"); good("log(1000)",3,"3");
    good("ln(e)",1,"1"); good("exp(1)",exp(1),"2.71828182846");
    good("ABS(-12)",12,"12"); good("sin(pi/6)",.5,"0.5");
    good("cos(0)",1,"1"); good("tan(pi/4)",1,"1");
    good("sqrt(1e-100)",1e-50,"1e-50");
    calc_context ctx = { .degrees = 1 };
    double value = 77; size_t at;
    assert(calc_eval_context("Ans", &ctx, &value, &at)); assert(value == 77);
    assert(!calc_eval_context("sin(30)", &ctx, &value, &at)); assert(fabs(value-.5)<1e-14);
    assert(!calc_eval_context("Ans*4", &ctx, &value, &at)); assert(fabs(value-2)<1e-14);
    assert(calc_eval_context("1/0", &ctx, &value, &at)); assert(fabs(ctx.ans-2)<1e-14);
    assert(calc_eval_context("tan(90)", &ctx, &value, &at)); assert(fabs(ctx.ans-2)<1e-14);
    ctx.degrees = 0;
    assert(!calc_eval_context("sin(pi/2)", &ctx, &value, &at)); assert(fabs(value-1)<1e-14);
    /* Compare a sweep against the host math library in both angle modes. */
    for (int deg=0; deg<2; deg++) {
        ctx.degrees=deg;
        for (int i=-100; i<=100; i++) {
            char expr[40]; snprintf(expr,sizeof expr,"sin(%d/10)",i);
            assert(!calc_eval_context(expr,&ctx,&value,&at));
            double angle=i/10.0; if (deg) angle*=acos(-1)/180;
            assert(fabs(value-sin(angle))<1e-14);
        }
    }
    const char *bad[]={"","1/0","2+","(1+2","1 2","1e","1e101","1e100*10","1e-100/10","0^0","2^.5","2^1001","1e100^1000","1+nan","sqrt(-1)","log(0)","ln(-1)","exp(1000)","exp(-1000)","sin(1e10)","sin()","sin(1,2)","cos 0","sqrt(4","pi2","Ans","unknown(1)","(((((((((((((((((1)))))))))))))))))"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) { double x=42; size_t at=999; assert(calc_eval(bad[i],&x,&at)); assert(x==42); assert(at<=strlen(bad[i])); }
    char buf[24]; assert(!calc_format(INFINITY,buf,sizeof buf)); assert(!calc_format(NAN,buf,sizeof buf)); assert(!calc_format(1,buf,2));
    puts("calc host tests PASS");
}
