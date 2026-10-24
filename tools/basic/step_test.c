#include "../../user/basic/basic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char output[8192]; static size_t used;
static void write_output(const char *s,size_t n,void *ctx) { (void)ctx;assert(used+n<sizeof output);memcpy(output+used,s,n);used+=n;output[used]=0; }
static void clear(void) { used=0;output[0]=0; }
static void run(const char *s,const char *error) {
    clear();ubasic_init(s,write_output,NULL,1);unsigned steps=0;
    while (!ubasic_finished()) {assert(++steps<1000);ubasic_run();}
    if (error) {if (!ubasic_error()) fprintf(stderr,"missing error: %s\n",s);assert(ubasic_error() && strstr(ubasic_error(),error));}
    else {if (ubasic_error()) fprintf(stderr,"%s\n",ubasic_error());assert(!ubasic_error());}
}
static void expect(const char *s) {if (strcmp(output,s)) fprintf(stderr,"got %s expected %s\n",output,s);assert(!strcmp(output,s));clear();}
int main(void) {
    run("1 PRINT INT(1.9),INT(-1.9),FIX(1.9),FIX(-1.9),INT(-.1),FIX(-.1)\n",NULL);expect("1 -2 1 -1 -1 0\n");
    run("1 DIM A(2)\n2 A(INT(1.9))=7\n3 PRINT A(FIX(1.9))\n",NULL);expect("7\n");
    run("1 PRINT INT(1e100),FIX(-1e100)\n",NULL);expect("1e+100 -1e+100\n");
    run("1 FOR X=0 TO .3 STEP .1\n2 PRINT X;\n3 NEXT X\n4 PRINT X\n",NULL);expect("00.10.20.30.4\n");
    run("1 FOR X=.3 TO 0 STEP -.1\n2 PRINT X;\n3 NEXT X\n4 PRINT X\n",NULL);expect("0.30.20.10-0.1\n");
    run("1 FOR X=.5 TO 2.5 STEP 1\n2 PRINT X,\n3 NEXT X\n",NULL);expect("0.5 1.5 2.5 ");
    run("1 FOR X=3 TO 1 STEP -1\n2 PRINT X;\n3 NEXT X\n",NULL);expect("321");
    run("1 FOR X=1 TO 3 STEP -1\n2 PRINT 999\n3 NEXT X\n4 PRINT 7\n",NULL);expect("7\n");
    run("1 FOR X=3 TO 1 STEP .1\n2 FOR Y=0 TO 1 STEP .5\n3 PRINT 999\n4 NEXT Y\n5 NEXT X\n6 PRINT 7\n",NULL);expect("7\n");
    run("1 FOR X=0 TO .1 STEP .1\n2 FOR Y=1 TO 0 STEP -.5\n3 PRINT X,Y\n4 NEXT Y\n5 NEXT X\n",NULL);expect("0 1\n0 0.5\n0 0\n0.1 1\n0.1 0.5\n0.1 0\n");
    run("1 C=0\n2 FOR X=0 TO 1 STEP .1\n3 C=C+1\n4 NEXT X\n5 PRINT C\n",NULL);expect("11\n");
    run("1 C=0\n2 FOR X=1 TO 0 STEP -.1\n3 C=C+1\n4 NEXT X\n5 PRINT C\n",NULL);expect("11\n");
    run("1 FOR X=1 TO 1 STEP -.1\n2 PRINT X\n3 NEXT X\n",NULL);expect("1\n");
    run("1 S=.25\n2 FOR X=0 TO S*2 STEP S\n3 PRINT X;\n4 NEXT X\n",NULL);expect("00.250.5");
    run("1 FOR X=0 TO 1 STEP 0\n2 NEXT X\n","STEP must be nonzero");
    run("1 FOR X=1e100 TO 1e100 STEP 1\n2 NEXT X\n","STEP does not advance");
    run("1 FOR X=1e100 TO 1e100 STEP 1e100\n2 NEXT X\n","numeric range exceeded");
    run("1 FOR X=0 TO 1 STEP\n2 NEXT X\n","expected number");
    run("1 FOR X=0 TO 1 STEP 1 STEP 2\n2 NEXT X\n","unexpected text");
    /* No STEP retains the previous checked integer behavior. */
    run("1 FOR X=.5 TO 2\n2 NEXT X\n","integer argument required");
    run("1 FOR X=0 TO 1\n2 X=.5\n3 NEXT X\n","integer argument required");
    run("1 FOR X=2147483647 TO 2147483647\n2 NEXT X\n","FOR counter out of range");
    basic_session *s=malloc(sizeof *s);assert(s);basic_session_init(s,write_output,NULL,NULL);
    const char *long_loop="10 FOR X=0 TO 1 STEP 1e-100\n20 NEXT X\n";
    unsigned at;assert(!basic_program_parse(&s->source,long_loop,strlen(long_loop),&at));clear();
    assert(basic_session_run(s)==-2 && strstr(output,"execution limit"));
    free(s);puts("BASIC INT/FIX and STEP loop tests PASS");
}
