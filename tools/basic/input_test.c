#include "../../user/basic/basic.h"
#include "../../user/basic/console.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char output[32768]; static size_t used;
static void write_output(const char *p,size_t n,void *opaque) {
    (void)opaque; assert(used+n<sizeof output); memcpy(output+used,p,n); used+=n; output[used]=0;
}
static void clear(void) { used=0; output[0]=0; }
static const char *replies[20]; static int statuses[20]; static unsigned reply_pos, reply_count;
static int read_reply(void *opaque,char *buf,size_t cap) {
    (void)opaque; assert(reply_pos<reply_count); unsigned i=reply_pos++;
    snprintf(buf,cap,"%s",replies[i]?replies[i]:""); return statuses[i];
}
static void script(const char *a,const char *b,const char *c) {
    memset(statuses,0,sizeof statuses); reply_pos=0; reply_count=0;
    if (a) replies[reply_count++]=a;
    if (b) replies[reply_count++]=b;
    if (c) replies[reply_count++]=c;
}
static void immediate(basic_session *s,const char *line) { assert(!basic_session_submit(s,line)); }
static void run(const char *src,const char *error) {
    clear(); ubasic_init(src,write_output,NULL,1);
    unsigned steps=0; while (!ubasic_finished() && !ubasic_waiting_input()) { assert(++steps<1000); ubasic_run(); }
    if (error) { assert(ubasic_error()); assert(strstr(ubasic_error(),error)); }
    else { if (ubasic_error()) fprintf(stderr,"unexpected: %s\n",ubasic_error()); assert(!ubasic_error()); }
}
static void expect(const char *s) {
    if (strcmp(output,s)) fprintf(stderr,"got: %s\nexpected: %s\n",output,s);
    assert(!strcmp(output,s)); clear();
}
static const int *keys; static unsigned keypos,keycount;
static int getkey(void *opaque) { (void)opaque; assert(keypos<keycount); return keys[keypos++]; }
static basic_console typed_console;
static int typed_cancel(void *opaque) {
    (void)opaque;
    int ch=keypos<keycount ? keys[keypos++] : -1;
    return basic_console_feed(&typed_console,ch);
}
static int typed_reply(void *opaque,char *buf,size_t cap) {
    (void)opaque;
    return basic_console_readline(&typed_console,buf,cap,getkey,write_output,NULL);
}
#define KEYS(...) const int seq[]={__VA_ARGS__}; keys=seq; keypos=0; keycount=sizeof seq/sizeof *seq

int main(void) {
    basic_session *s=malloc(sizeof *s); assert(s);
    basic_session_init(s,write_output,NULL,NULL); s->input=read_reply;
    immediate(s,"DIM A(3),B(1)"); immediate(s,"A=77");
    immediate(s,"A(0)=12"); immediate(s,"LET A(3)=-9"); immediate(s,"B(1)=A(0)+A(3)");
    immediate(s,"PRINT A,A(0),A(1),A(3),B(1)"); expect("77 12 0 -9 3\n");
    immediate(s,"I=3"); immediate(s,"PRINT A(I),A(B(1))"); expect("-9 -9\n");
    script("42",NULL,NULL); immediate(s,"INPUT N"); assert(reply_pos==1 && ubasic_get_variable(13)==42); expect("? ");
    script("-2147483648",NULL,NULL); immediate(s,"INPUT \"N\"; N"); assert(ubasic_get_variable(13)==INT32_MIN); expect("N? ");
    script(" +2147483647 \t",NULL,NULL); immediate(s,"INPUT N"); assert(ubasic_get_variable(13)==INT32_MAX); expect("? ");
    script("1e101","1+2","19"); immediate(s,"INPUT A(I)"); assert(reply_pos==3);
    assert(strstr(output,"range exceeded") && strstr(output,"expected decimal")); clear();
    immediate(s,"PRINT A(3),A"); expect("19 77\n");
    script("",NULL,NULL); statuses[0]=-1; immediate(s,"INPUT A(0)"); assert(strstr(output,"Break")); clear();
    immediate(s,"PRINT A(0)"); expect("12\n"); assert(!ubasic_waiting_input());
    script("",NULL,NULL); statuses[0]=-2; immediate(s,"INPUT A"); assert(strstr(output,"input read failed")); assert(ubasic_get_variable(0)==77); clear();
    script("truncated","-23",NULL); statuses[0]=1; immediate(s,"INPUT A"); assert(ubasic_get_variable(0)==-23); assert(strstr(output,"Redo")); clear();
    s->input=NULL; immediate(s,"INPUT A"); assert(strstr(output,"INPUT is unavailable")); clear(); s->input=read_reply;
    /* Array state is preserved in immediate mode and cleared by NEW / RUN. */
    immediate(s,"NEW"); immediate(s,"PRINT A(0)"); assert(strstr(output,"not DIMensioned")); clear();
    immediate(s,"DIM A(0)"); immediate(s,"A(0)=99");
    immediate(s,"10 DIM A(0)"); immediate(s,"20 PRINT A(0)"); assert(!basic_session_run(s)); expect("0\n");
    assert(!basic_session_run(s)); expect("0\n");
    run("1 DIM A(1023)\n2 A(1023)=17\n3 PRINT A(0),A(1023)\n",NULL); expect("0 17\n");
    run("1 DIM A(511),B(511)\n2 PRINT B(511)\n",NULL); expect("0\n");
    run("1 DIM A(1024)\n","storage full");
    run("1 DIM A(1023),B(0)\n","storage full");
    run("1 DIM A(-1)\n","negative array bound");
    run("1 DIM A(1),A(2)\n","already DIMensioned");
    run("1 A(0)=1\n","not DIMensioned");
    run("1 DIM A(0)\n2 PRINT A(-1)\n","index out of range");
    run("1 DIM A(0)\n2 A(1)=2\n","index out of range");
    run("1 DIM A(0)\n2 PRINT A(2147483647)\n","index out of range");
    run("1 DIM A(0)\n2 PRINT A(A(A(A(A(A(A(A(A(A(A(A(A(A(A(A(A(0)))))))))))))))))\n","too deep");
    run("1 DIM A(0)\n2 INPUT A(-1)\n","index out of range"); assert(!ubasic_waiting_input());
    run("1 INPUT A,B\n","unexpected text"); assert(!ubasic_waiting_input());
    run("1 INPUT \"N\" A\n","expected ';'");
    run("1 INPUT 3\n","expected variable");
    /* A final-line INPUT must stay runnable until a valid reply arrives. */
    run("1 A=9\n2 INPUT A",NULL); assert(ubasic_waiting_input() && !ubasic_finished());
    ubasic_run(); assert(ubasic_waiting_input() && ubasic_get_variable(0)==9);
    const char *invalid[]={"","-","+","0x10","1e101","1e-101","nan","inf","1e+","1 2"};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) { assert(ubasic_supply_input(invalid[i])); assert(ubasic_get_variable(0)==9 && ubasic_waiting_input()); }
    assert(!ubasic_supply_input("-0")); assert(ubasic_get_variable(0)==0 && ubasic_finished());
    assert(ubasic_supply_input("1"));
    run("1 IF 0 THEN INPUT A ELSE PRINT 7\n",NULL); expect("7\n");
    run("1 IF 1 THEN INPUT A ELSE PRINT 7\n",NULL); assert(ubasic_waiting_input());
    assert(!ubasic_supply_input("5")); assert(ubasic_get_variable(0)==5 && ubasic_finished()); clear();
    /* Complete example: array input inside FOR and file-parser round trip. */
    basic_session_init(s,write_output,NULL,NULL); s->input=read_reply;
    FILE *fp=fopen("user/basic/examples/input_sum.bas","rb"); assert(fp);
    char source[1024]; size_t n=fread(source,1,sizeof source,fp); assert(feof(fp)&&!ferror(fp)); fclose(fp);
    unsigned line; assert(!basic_program_parse(&s->source,source,n,&line));
    script("2","3","4"); assert(!basic_session_run(s)); assert(reply_pos==3 && ubasic_get_variable(18)==9);
    clear(); immediate(s,"PRINT A(2)"); expect("4\n");
    script("2","",NULL); statuses[1]=-1; assert(basic_session_run(s)==-2); assert(ubasic_get_variable(18)==2); clear();
    /* Typeahead, editing, CRLF, overlong lines and cancellation at console level. */
    basic_console c={0}; char buf[128];
    basic_console_feed(&c,'1'); basic_console_feed(&c,'2'); basic_console_feed(&c,'\r'); basic_console_feed(&c,'\n');
    { KEYS('3','\n'); assert(!basic_console_readline(&c,buf,sizeof buf,getkey,write_output,NULL)); assert(!strcmp(buf,"12") && !keypos);
      assert(!basic_console_readline(&c,buf,sizeof buf,getkey,write_output,NULL)); assert(!strcmp(buf,"3")); }
    clear();
    { KEYS('1','2',8,'3',21,'-','7','\n'); assert(!basic_console_readline(&c,buf,sizeof buf,getkey,write_output,NULL)); assert(!strcmp(buf,"-7")); }
    { KEYS('8',3); assert(basic_console_readline(&c,buf,sizeof buf,getkey,write_output,NULL)==-1 && !buf[0]); }
    basic_console_feed(&c,'7'); assert(basic_console_feed(&c,3) && !c.count);
    for (unsigned i=0;i<sizeof c.queue+1;i++) basic_console_feed(&c,'1');
    assert(c.overflow && basic_console_readline(&c,buf,sizeof buf,getkey,write_output,NULL)==-2 && !c.count);
    { KEYS('1','2','3','4','5','\n'); assert(basic_console_readline(&c,buf,4,getkey,write_output,NULL)==1); }
    { KEYS('1',27,'2','\n'); assert(basic_console_readline(&c,buf,sizeof buf,getkey,write_output,NULL)==1); }
    { KEYS(-1); assert(basic_console_readline(&c,buf,sizeof buf,getkey,write_output,NULL)==-2); }
    /* Polling for Ctrl-C before INPUT must preserve the first typed character. */
    clear(); memset(&typed_console,0,sizeof typed_console);
    basic_session_init(s,write_output,typed_cancel,NULL); s->input=typed_reply;
    immediate(s,"10 INPUT A"); immediate(s,"20 INPUT B"); immediate(s,"30 PRINT A+B");
    { KEYS('1','2','\r','\n','3','4','\r','\n'); assert(!basic_session_run(s));
      assert(ubasic_get_variable(0)==12 && ubasic_get_variable(1)==34 && strstr(output,"46\n")); }
    clear();
    { KEYS('9',3); assert(basic_session_run(s)==-2); assert(!ubasic_get_variable(0) && !ubasic_waiting_input()); }
    clear();
    /* Structured malformed arrays / INPUT exercise the new keyword paths. */
    const char *fragments[]={"-1","1024","2147483647","1/0","(","A(0)","0,0","0) A(0)=9","0"};
    for (unsigned i=0;i<sizeof fragments/sizeof *fragments;i++) {
        char program[128]; snprintf(program,sizeof program,"1 DIM A(%s)\n2 INPUT A(%s)\n",fragments[i],fragments[i]);
        ubasic_init(program,write_output,NULL,1);
        for (unsigned j=0;j<5 && !ubasic_finished() && !ubasic_waiting_input();j++) ubasic_run();
        ubasic_stop(); clear();
    }
    free(s); puts("BASIC INPUT/array/console tests PASS");
}
