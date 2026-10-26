#include "quakedef.h"
#include <m5os/hal_time.h>
#include <m5os/task.h>
qboolean isDedicated;
static FILE *handles[10];
int Sys_FileOpenRead(char *path,int *out) {
    *out=-1;
    for(int i=1;i<10;i++) if(!handles[i]) {
        FILE *f=fopen(path,"rb");if(!f)return -1;
        if(fseek(f,0,SEEK_END)) { fclose(f);return -1; }
        long n=ftell(f);if(n<0||fseek(f,0,SEEK_SET)) {fclose(f);return -1;}
        handles[i]=f;*out=i;return (int)n;
    }
    Sys_Error("file handle limit");return -1;
}
int Sys_FileOpenWrite(char *path) { (void)path;return -1; }
void Sys_FileClose(int h) { if(h>0&&h<10&&handles[h]) {fclose(handles[h]);handles[h]=NULL;} }
void Sys_FileSeek(int h,int p) { if(h<=0||h>=10||!handles[h]||fseek(handles[h],p,SEEK_SET))Sys_Error("file seek"); }
int Sys_FileRead(int h,void *p,int n) { return h>0&&h<10&&handles[h]&&n>=0?(int)fread(p,1,n,handles[h]):-1; }
int Sys_FileWrite(int h,void *p,int n) { (void)h;(void)p;(void)n;return -1; }
int Sys_FileTime(char *path) { FILE *f=fopen(path,"rb");if(!f)return -1;fclose(f);return 1; }
void Sys_mkdir(char *path) { (void)path; }
void Sys_MakeCodeWriteable(unsigned long p,unsigned long n) { (void)p;(void)n; }
void Sys_Error(char *fmt,...) { va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);exit(1); }
void Sys_Printf(char *fmt,...) { va_list a;va_start(a,fmt);vfprintf(stdout,fmt,a);va_end(a); }
void Sys_DebugLog(char *file,char *fmt,...) { (void)file;(void)fmt; }
void Sys_Quit(void) { exit(0); }
double Sys_FloatTime(void) { return hal_time_now_us()/1000000.0; }
char *Sys_ConsoleInput(void) { return NULL; }
void Sys_Sleep(void) { task_sleep(1); }
void Sys_SendKeyEvents(void) {}
void Sys_HighFPPrecision(void) {}
void Sys_LowFPPrecision(void) {}
void Sys_SetFPCW(void) {}
