#include "../../user/basic/basic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char output[32768]; static size_t used; static unsigned polls, break_after;
static void write_output(const char *p, size_t n, void *ctx) {
    (void)ctx; assert(used+n < sizeof output); memcpy(output+used,p,n); used+=n; output[used]=0;
}
static int cancel(void *ctx) { (void)ctx; return break_after && ++polls >= break_after; }
static void clear(void) { used=0; output[0]=0; polls=0; break_after=0; }
static void expect(const char *s) {
    if (strcmp(output,s)) fprintf(stderr,"got:\n%s\nexpected:\n%s\n",output,s);
    assert(!strcmp(output,s)); clear();
}
static void submit(basic_session *s, const char *line) { assert(!basic_session_submit(s,line)); }
static void engine(const char *program, const char *expected_error) {
    clear(); ubasic_init(program,write_output,NULL,1);
    unsigned steps=0; while (!ubasic_finished()) { assert(++steps<10000); ubasic_run(); }
    if (expected_error) { assert(ubasic_error()); assert(strstr(ubasic_error(),expected_error)); }
    else { if (ubasic_error()) fprintf(stderr,"%s\n",ubasic_error()); assert(!ubasic_error()); }
}
static unsigned file_calls;
static int file_action;
static char file_path[BASIC_LINE_CAP];
static int file_command(void *opaque, int action, const char *path) {
    (void)opaque; file_calls++; file_action=action; strcpy(file_path,path); return 0;
}
int main(void) {
    basic_session *s=malloc(sizeof *s); assert(s);
    basic_session_init(s,write_output,cancel,NULL);
    submit(s,"PRINT 2+3*4"); expect("14\n");
    submit(s,"LET A = 12"); submit(s,"print a/5, -a, (a+2)*3"); expect("2.4 -12 42\n");
    submit(s,"PRINT -2147483647-1"); expect("-2147483648\n");
    submit(s,"30 NEXT I"); submit(s,"10 FOR I=1 TO 3"); submit(s,"20 PRINT I;"); submit(s,"40 END");
    submit(s,"LIST"); expect("10 FOR I=1 TO 3\n20 PRINT I;\n30 NEXT I\n40 END\n");
    submit(s,"RUN"); expect("123");
    submit(s,"PRINT I"); expect("4\n");
    submit(s,"20 PRINT I"); submit(s,"RUN"); expect("1\n2\n3\n");
    submit(s,"20"); submit(s,"20 PRINT I*I"); submit(s,"RUN"); expect("1\n4\n9\n");
    submit(s,"NEW"); submit(s,"PRINT A"); expect("0\n");
    submit(s,"10 GOSUB 100"); submit(s,"20 IF A=42 THEN PRINT \"OK\" ELSE PRINT \"BAD\"");
    submit(s,"30 END"); submit(s,"100 A=42"); submit(s,"110 RETURN"); submit(s,"RUN"); expect("OK\n");
    submit(s,"IF 2<=3 THEN PRINT \"yes\" ELSE PRINT \"no\""); expect("yes\n");
    submit(s,"IF 2<>2 THEN PRINT \"yes\" ELSE PRINT \"no\""); expect("no\n");
    submit(s,"IF 4>=3 THEN PRINT 1"); expect("1\n");
    submit(s,"REM trailing comment without quotes \""); expect("");
    submit(s,"NEW"); submit(s,"10 FOR I=5 TO 3"); submit(s,"20 PRINT 999"); submit(s,"30 NEXT I");
    submit(s,"40 PRINT 7"); submit(s,"RUN"); expect("7\n");
    submit(s,"NEW"); submit(s,"10 GOTO 10"); break_after=10;
    submit(s,"RUN"); expect("Break\n");
    submit(s,"RUN"); expect("Break: 100000-line execution limit\n");
    submit(s,"NEW"); submit(s,"10 GOTO 999"); submit(s,"RUN"); assert(strstr(output,"no such line")); clear();
    submit(s,"PRINT \"recovered\""); expect("recovered\n");
    submit(s,"NEW");
    for (unsigned i=1;i<=128;i++) { char line[40]; snprintf(line,sizeof line,"%u REM X",i); submit(s,line); }
    submit(s,"129 PRINT 1"); assert(strstr(output,"program full")); clear();
    submit(s,"128 PRINT 128"); submit(s,"RUN"); expect("128\n");
    submit(s,"65536 PRINT 1"); assert(strstr(output,"line number")); clear();
    char long_line[140]; memset(long_line,'X',sizeof long_line); long_line[139]=0;
    submit(s,long_line); assert(strstr(output,"too long")); clear();
    s->file=file_command;
    submit(s,"load \"/sd/My File.bas\""); assert(file_calls==1 && file_action==BASIC_LOAD && !strcmp(file_path,"/sd/My File.bas"));
    submit(s,"SAVE ../Demo.bas"); assert(file_calls==2 && file_action==BASIC_SAVE && !strcmp(file_path,"../Demo.bas"));
    submit(s,"edit"); assert(file_calls==3 && file_action==BASIC_EDIT && !*file_path);
    submit(s,"LOAD \"unterminated"); assert(file_calls==3); clear();
    submit(s,"LOAD \"\""); assert(file_calls==3); clear();
    submit(s,"SAVE \"a\" extra"); assert(file_calls==3); clear();
    assert(basic_session_submit(s,"bye"));
    engine("1 PRINT 1/0\n","division by zero");
    engine("1 PRINT 1%0\n","division by zero");
    engine("1 PRINT 2147483647+1\n",NULL); expect("2147483648\n");
    engine("1 PRINT 50000*50000\n",NULL); expect("2500000000\n");
    engine("1 PRINT (-2147483647-1)/-1\n",NULL); expect("2147483648\n");
    engine("1 PRINT -(-2147483647-1)\n",NULL); expect("2147483648\n");
    engine("1 PRINT 1e100*10\n","numeric range exceeded");
    engine("1 PRINT 1e101\n","numeric range exceeded");
    engine("1 PRINT \"unterminated\n","unterminated");
    engine("1 PRINT @\n","invalid character");
    engine("1 PRINT (((((((((((((((((1)))))))))))))))))\n","too deep");
    engine("1 RETURN\n","RETURN without GOSUB");
    engine("1 IF 0 THEN IF 1 THEN PRINT 1 ELSE PRINT 2\n", "nested inline IF");
    engine("1 IF 1 THEN PRINT 1 ELSE IF 1 THEN PRINT 2\n", "nested inline IF");
    engine("1 NEXT A\n","NEXT without matching");
    engine("1 GOSUB 1\n","GOSUB stack full");
    engine("1 FOR I=1 TO 2\n","FOR without NEXT");
    engine("1 FOR I=2 TO 1\n","FOR without NEXT");
    engine("1 PEEK 0,A\n","unknown keyword");
    engine("1 PRINT 1\n1 PRINT 2\n","ascending");
    engine("1 FOR I=1 TO 2\n2 FOR J=1 TO 2\n3 PRINT I,J\n4 NEXT J\n5 NEXT I\n",NULL);
    expect("1 1\n1 2\n2 1\n2 2\n");
    engine("1 FOR I=2 TO 1\n2 FOR J=1 TO 2\n3 PRINT 999\n4 NEXT J\n5 NEXT I\n6 PRINT 8\n",NULL);
    expect("8\n");
    engine("1 FOR I=1 TO 2\n2 FOR J=2 TO 1\n3 PRINT 999\n4 NEXT J\n5 PRINT I\n6 NEXT I\n",NULL);
    expect("1\n2\n");
    engine("1 FOR I=1 TO 2\n2 FOR J=1 TO 2\n3 FOR K=1 TO 2\n4 FOR L=1 TO 2\n5 FOR M=1 TO 2\n", "FOR stack full");
    engine("1 REM comment at EOF",NULL);
    engine("1 PRINT 7",NULL); expect("7\n");
    /* Deterministic malformed-input campaign: every step must return. */
    unsigned rng=1;
    for (unsigned trial=0;trial<5000;trial++) {
        char program[128]="1 "; unsigned n=2;
        while (n<sizeof program-2) { rng=rng*1664525u+1013904223u; program[n++]=(char)(32+(rng>>24)%95); }
        program[n++]='\n'; program[n]=0;
        clear(); ubasic_init(program,write_output,NULL,1);
        for (int step=0;step<20 && !ubasic_finished();step++) ubasic_run();
    }
    free(s); puts("BASIC host tests PASS");
}
