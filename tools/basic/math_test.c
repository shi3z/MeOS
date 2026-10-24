#include "../../user/basic/basic.h"
#include <m5os/number_math.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char output[8192]; static size_t used;
static void write_output(const char *p,size_t n,void *ctx) { (void)ctx; assert(used+n<sizeof output); memcpy(output+used,p,n); used+=n;output[used]=0; }
static void clear(void) { used=0;output[0]=0; }
static void expect(const char *s) { if (strcmp(output,s)) fprintf(stderr,"got %s expected %s\n",output,s); assert(!strcmp(output,s));clear(); }
static void run(const char *src,const char *error) {
    clear(); ubasic_init(src,write_output,NULL,1);
    unsigned steps=0; while (!ubasic_finished()) { assert(++steps<1000);ubasic_run(); }
    if (error) { assert(ubasic_error());assert(strstr(ubasic_error(),error)); }
    else { if (ubasic_error()) fprintf(stderr,"unexpected: %s\n",ubasic_error()); assert(!ubasic_error()); }
}
int main(void) {
    run("1 PRINT SIN(PI/6),COS(0),TAN(PI/4)\n",NULL);expect("0.5 1 1\n");
    run("1 PRINT SQR(9),SQRT(16),ABS(-3.5),EXP(0)\n",NULL);expect("3 4 3.5 1\n");
    run("1 PRINT LOG(EXP(1)),LN(EXP(1)),LOG10(1000)\n",NULL);expect("1 1 3\n");
    run("1 PRINT 2^3^2,-2^2,(-2)^2,2^-3,4^.5\n",NULL);expect("512 -4 4 0.125 2\n");
    run("1 DEG\n2 PRINT SIN(30),COS(60),TAN(45)\n3 RAD\n4 PRINT SIN(PI/2)\n",NULL);expect("0.5 0.5 1\n1\n");
    run("1 DIM A(0)\n2 A(0)=SQR(2)^2\n3 IF ABS(A(0)-2)<1e-12 THEN PRINT 7 ELSE PRINT 0\n",NULL);expect("7\n");
    const char *bad[]={"1 PRINT SQR(-1)\n","1 PRINT LOG(0)\n","1 PRINT LOG10(-1)\n","1 PRINT EXP(1000)\n","1 PRINT SIN(1e10)\n","1 DEG\n2 PRINT TAN(90)\n","1 PRINT (-2)^.5\n","1 PRINT 0^0\n","1 PRINT 0^-1\n","1 PRINT 1e100^2\n","1 PRINT 1e-100^2\n","1 PRINT SIN()\n","1 PRINT COS 0\n","1 PRINT LOG(1,2)\n","1 DEG 1\n"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
        clear();ubasic_init(bad[i],write_output,NULL,1);
        unsigned steps=0;while (!ubasic_finished()) {assert(++steps<10);ubasic_run();}
        assert(ubasic_error());
    }
    run("1 PRINT SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(1)))))))))))))))))\n","too deep");
    basic_session *s=malloc(sizeof *s);assert(s);basic_session_init(s,write_output,NULL,NULL);
    clear();basic_session_submit(s,"DEG");basic_session_submit(s,"PRINT SIN(30)");expect("0.5\n");
    basic_session_submit(s,"10 PRINT SIN(PI/2)");assert(!basic_session_run(s));expect("1\n");
    /* Shared helper results against host libm, with output preservation on errors. */
    for (int mode=0;mode<2;mode++) for (int i=-100;i<=100;i++) {
        double x=i/10.0, result;double angle=mode ? x*NUMBER_PI/180 : x;
        assert(!number_math(NUMBER_SIN,x,mode,&result) && fabs(result-sin(angle))<1e-14);
        assert(!number_math(NUMBER_COS,x,mode,&result) && fabs(result-cos(angle))<1e-14);
    }
    double result=42;assert(number_math(NUMBER_LOG,0,0,&result) && result==42);
    assert(number_math(NUMBER_SQRT,-1,0,&result) && result==42);
    assert(number_power(-2,.5,&result) && result==42);
    assert(!number_power(1e100,-1,&result) && result>0.99999999999999e-100);
    free(s);puts("BASIC/shared math functions and powers tests PASS");
}
