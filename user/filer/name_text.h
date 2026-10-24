#pragma once
#include <m5os/types.h>
/* Preserve the complete VFS name separately; this label never splits UTF-8. */
static inline void filer_name_label(const char *src,char out[96],unsigned cells) {
    unsigned n=0,width=0;bool clipped=false;
    while(*src) {
        unsigned char c=(unsigned char)*src;unsigned k=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
        unsigned w=c<0x80?1:2;
        if(width+w>cells || n+k>=92){clipped=true;break;}
        bool valid=c<0x80 || (c>=0xc2 && c<=0xf4);
        for(unsigned j=1;j<k && valid;j++)if(!src[j] || ((unsigned char)src[j]&0xc0)!=0x80)valid=false;
        if(!valid){out[n++]='?';src++;width++;continue;}
        for(unsigned j=0;j<k;j++)out[n++]=*src++;
        width+=w;
    }
    if(clipped && n<95 && width<cells){out[n++]='~';width++;}
    while(width<cells && n<95){out[n++]=' ';width++;}
    out[n]=0;
}
