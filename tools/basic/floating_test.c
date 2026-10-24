#include "../../user/basic/basic.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char output[8192]; static size_t used;
static void out(const char *p,size_t n,void *ctx) { (void)ctx; assert(used+n<sizeof output); memcpy(output+used,p,n); used+=n;output[used]=0; }
static void clear(void) { used=0; output[0]=0; }
static void expect(const char *s) { if (strcmp(output,s)) fprintf(stderr,"got %s expected %s\n",output,s); assert(!strcmp(output,s)); clear(); }
static void source(const char *s,const char *error) {
    clear(); ubasic_init(s,out,NULL,1);
    unsigned steps=0; while (!ubasic_finished() && !ubasic_waiting_input()) { assert(++steps<1000); ubasic_run(); }
    if (error) { assert(ubasic_error()); assert(strstr(ubasic_error(),error)); }
    else assert(!ubasic_error());
}
static const char *replies[3]; static unsigned reply;
static int input(void *ctx,char *buf,size_t cap) { (void)ctx; assert(reply<3); snprintf(buf,cap,"%s",replies[reply++]); return 0; }
int main(void) {
    source("1 PRINT 12/5,-7/2,.5+1.25,1e-3*2E2\n",NULL); expect("2.4 -3.5 1.75 0.2\n");
    source("1 DIM A(1)\n2 A(0)=.125\n3 A(1)=A(0)*3\n4 PRINT A(0),A(1),A(1)/3\n",NULL); expect("0.125 0.375 0.125\n");
    source("1 IF .5+.25=.75 THEN PRINT 1 ELSE PRINT 0\n",NULL); expect("1\n");
    source("1 PRINT 1e100,1e-100\n",NULL); expect("1e+100 1e-100\n");
    source("1 PRINT 2147483647+1,50000*50000\n",NULL); expect("2147483648 2500000000\n");
    source("1 PRINT -2147483648%-1,-7%3,6&3,4|1\n",NULL); expect("0 -1 2 5\n");
    const char *bad[]={
        "1 PRINT 1e100*10\n","1 PRINT 1e-100/10\n","1 PRINT 1e100/1e-100\n",
        "1 PRINT 1e-100*1e-100\n","1 PRINT 1e100+1e100\n"
    };
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) source(bad[i],"numeric range exceeded");
    const char *integers[]={
        "1 DIM A(1.5)\n","1 DIM A(2)\n2 A(.5)=1\n","1 DIM A(2)\n2 PRINT A(1e100)\n",
        "1 GOTO 10.5\n","1 GOSUB 1e100\n","1 FOR I=.5 TO 2\n","1 FOR I=0 TO 1.5\n",
        "1 FOR I=0 TO 1\n2 I=.5\n3 NEXT I\n","1 PRINT 1.5%1\n","1 PRINT 1%1.5\n",
        "1 PRINT 1.5&1\n","1 PRINT 1|.5\n","1 PRINT 2147483648&1\n",
        "1 SCREEN .5\n","1 CLS .5\n","1 PSET .5,0,1\n","1 LINE 0,0,1.5,1,3\n",
        "1 TEXT 0,0,\"x\",.5\n","1 WAIT .5\n"
    };
    for (unsigned i=0;i<sizeof integers/sizeof *integers;i++) source(integers[i],"integer argument required");
    source("1 FOR I=2147483647 TO 2147483647\n2 NEXT I\n","FOR counter out of range");
    source("1 GOTO 20.0\n20 PRINT .25\n",NULL); expect("0.25\n");
    source("1 INPUT A\n",NULL); assert(ubasic_waiting_input());
    assert(ubasic_supply_input("1e101")); assert(ubasic_waiting_input() && !ubasic_get_variable(0));
    assert(!ubasic_supply_input(" -1.25e-3 \t")); assert(fabs(ubasic_get_variable(0)+.00125)<1e-16);
    basic_session *s=malloc(sizeof *s); assert(s); basic_session_init(s,out,NULL,NULL); s->input=input;
    FILE *fp=fopen("user/basic/examples/average.bas","rb"); assert(fp);
    char text[1024]; size_t len=fread(text,1,sizeof text,fp); assert(feof(fp)&&!ferror(fp)); fclose(fp);
    unsigned line; assert(!basic_program_parse(&s->source,text,len,&line));
    replies[0]="1";replies[1]="1.5";replies[2]="2e0";reply=0;clear();
    assert(!basic_session_run(s)); assert(strstr(output,"AVERAGE = 1.5\n") && ubasic_get_variable(18)==4.5);
    clear(); assert(!basic_session_submit(s,"PRINT A(1)")); expect("1.5\n");
    free(s); puts("BASIC floating-point/boundary tests PASS");
}
