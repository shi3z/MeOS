/* Host-only capture of real BASIC + graphics adapter dispatch as SVG.
 * This is not an ESP32/display-driver emulator: WAIT has no wall-clock delay,
 * and the captured frame excludes the console repaint after SCREEN 0. */
#include "../../user/basic/basic.h"
#include "../../user/basic/graphics.h"
#include <m5os/display.h>
#include <m5os/arch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *scene;
static int paused;
static unsigned pixels, segments, captions, frames;
uint32_t arch_irq_disable(void) { return 1; }
void arch_irq_restore(uint32_t flags) { (void)flags; }
bool display_is_ready(void) { return true; }
bool fbcon_is_paused(void) { return paused; }
void fbcon_pause(bool value) { paused=value; }
void ime_bypass(bool value) { (void)value; }
void task_sleep(uint32_t milliseconds) { (void)milliseconds; }
void display_set_target(uint16_t *p) { assert(!p); }
static unsigned rgb(uint16_t c)
{
    unsigned r=((c>>11)&31)*255/31,g=((c>>5)&63)*255/63,b=(c&31)*255/31;
    return r<<16|g<<8|b;
}
void display_fill_rect(int x,int y,int w,int h,uint16_t c)
{
    fprintf(scene,"<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"#%06x\"/>\n",x,y,w,h,rgb(c));
    if(w==1 && h==1) pixels++;
}
void display_clear(uint16_t c) { display_fill_rect(0,0,DISPLAY_WIDTH,DISPLAY_HEIGHT,c);frames++; }
void display_draw_line(int x,int y,int x2,int y2,uint16_t c)
{
    fprintf(scene,"<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#%06x\"/>\n",x,y,x2,y2,rgb(c));segments++;
}
static void xml(const char *s)
{
    for(;*s;s++) {
        switch(*s) {
        case '&': fputs("&amp;",scene);break;
        case '<': fputs("&lt;",scene);break;
        case '>': fputs("&gt;",scene);break;
        default: fputc((unsigned char)*s,scene);break;
        }
    }
}
void display_draw_text(int x,int y,const char *s,uint16_t fg,uint16_t bg,int scale)
{
    display_fill_rect(x,y,(int)strlen(s)*8*scale,8*scale,bg);
    fprintf(scene,"<text x=\"%d\" y=\"%d\" fill=\"#%06x\" font-family=\"monospace\" font-size=\"%d\">",x,y+7*scale,rgb(fg),8*scale);
    xml(s);fputs("</text>\n",scene);captions++;
}
static void console(const char *s,size_t n,void *ctx) { (void)ctx;fwrite(s,1,n,stderr); }
static const char *draw(void *ctx,int op,const int32_t a[5],const char *s,size_t n)
{
    return basic_graphics_command(ctx,op,a,s,n,NULL,NULL);
}
int main(int argc,char **argv)
{
    if(argc!=3) {fprintf(stderr,"usage: render SOURCE.bas OUTPUT.svg\n");return 2;}
    FILE *input=fopen(argv[1],"rb");if(!input) {perror(argv[1]);return 1;}
    char *source=malloc(BASIC_PROGRAM_CAP);basic_session *session=malloc(sizeof *session);
    if(!source || !session) {fprintf(stderr,"out of memory\n");fclose(input);free(source);free(session);return 1;}
    size_t size=fread(source,1,BASIC_PROGRAM_CAP,input);int read_error=ferror(input);fclose(input);
    basic_graphics graphics={0};basic_session_init(session,console,NULL,&graphics);session->graphics=draw;
    unsigned line;const char *error=read_error ? "cannot read source" : basic_program_parse(&session->source,source,size,&line);
    if(error) {fprintf(stderr,"source error: %s\n",error);free(source);free(session);return 1;}
    scene=tmpfile();if(!scene) {perror("tmpfile");free(source);free(session);return 1;}
    fprintf(scene,"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1280\" height=\"720\" viewBox=\"0 0 1280 720\">\n");
    int status=basic_session_run(session);
    fputs("</svg>\n",scene);fflush(scene);
    if(status || graphics.active || paused || !frames || ferror(scene)) {
        fprintf(stderr,"render failed: program status=%d, frames=%u\n",status,frames);
        fclose(scene);free(source);free(session);return 1;
    }
    rewind(scene);FILE *out=fopen(argv[2],"wb");
    if(!out) {perror(argv[2]);fclose(scene);free(source);free(session);return 1;}
    char buffer[4096];size_t n;int io_error=0;
    while((n=fread(buffer,1,sizeof buffer,scene))) if(fwrite(buffer,1,n,out)!=n) {io_error=1;break;}
    if(ferror(scene)) io_error=1;
    if(fclose(out)) io_error=1;
    fclose(scene);free(source);free(session);
    if(io_error) {fprintf(stderr,"cannot finish SVG output\n");return 1;}
    printf("SVG captured: %u pixels, %u lines, %u labels\n",pixels,segments,captions);
    return 0;
}
