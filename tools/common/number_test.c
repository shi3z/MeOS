#include <m5os/number.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void parse(const char *text,double want,const char *suffix) {
    const char *end; double n=42;
    const char *error=number_parse(text,&end,&n);
    if (error) fprintf(stderr,"%s: %s\n",text,error);
    assert(!error && !strcmp(end,suffix)); assert(fabs(n-want)<=fabs(want)*1e-14+1e-110);
}
int main(void) {
    parse("1.25e-3",.00125,""); parse(".5+1",.5,"+1"); parse("-12.5",-12.5,"");
    parse("2147483647",2147483647,""); parse("-2147483648",-2147483648.0,"");
    parse("00000000000000000001.25",1.25,""); parse("1e100",1e100,""); parse("1e-100",1e-100,"");
    parse("0.1e101",1e100,""); parse("10000000000000000e-116",1e-100,"");
    parse("-0e4096",0,""); parse("5.",5,""); parse("1e+2x",100,"x");
    const char *bad[]={"",".","+","--1","1e","1e+","nan","inf","1e101","1e-101","1e9999999999999999"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) { double n=42; const char *end;
        assert(number_parse(bad[i],&end,&n)); assert(n==42 && end>=bad[i] && end<=bad[i]+strlen(bad[i])); }
    int32_t n=17;
    assert(number_int32(2147483647,&n) && n==INT32_MAX); assert(number_int32(-2147483648.0,&n) && n==INT32_MIN);
    const double invalid[]={NAN,INFINITY,-INFINITY,2147483648.0,-2147483649.0,.1,-.1,1e100};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) { n=17; assert(!number_int32(invalid[i],&n) && n==17); }
    char buf[24]; assert(!number_format(NAN,buf,sizeof buf)); assert(!number_format(1,buf,23));
    unsigned rng=7;
    for (unsigned i=0;i<10000;i++) {
        rng=rng*1664525u+1013904223u; unsigned lead=1+rng%9;
        rng=rng*1664525u+1013904223u; unsigned fraction=rng%1000000000u;
        int exp=(int)(rng%199)-100;
        char text[80]; snprintf(text,sizeof text,"%u.%09ue%+d",lead,fraction,exp);
        double value; const char *end; assert(!number_parse(text,&end,&value) && !*end);
        double reference=strtod(text,NULL); assert(fabs(value-reference)<=fabs(reference)*1e-14);
        assert(number_format(value,buf,sizeof buf)); double roundtrip;
        assert(!number_parse(buf,&end,&roundtrip) && !*end);
        assert(fabs(roundtrip-reference)<=fabs(reference)*6e-12);
    }
    puts("shared number parsing/format/conversion tests PASS");
}
