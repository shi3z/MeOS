#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <m5os/errno.h>
#include "../../user/player/mp3_stream.h"
struct reader{const unsigned char *data;size_t size,pos,limit;unsigned reads,fail_at;};
static int read_input(void *ctx,void *out,size_t len){
    struct reader *r=ctx;if(++r->reads==r->fail_at)return ERR_IO;
    if(len>r->limit)len=r->limit;
    if(len>r->size-r->pos)len=r->size-r->pos;
    memcpy(out,r->data+r->pos,len);r->pos+=len;return (int)len;
}
static int seek_input(void *ctx,uint32_t offset){struct reader *r=ctx;r->pos=offset>r->size?r->size:offset;return (int)r->pos;}
static int run(const unsigned char *data,size_t size,size_t limit,FILE *output,unsigned *rate,unsigned *channels){
    struct reader r={data,size,0,limit,0,0};mp3_stream_t *s=malloc(mp3_stream_bytes());assert(s);
    mp3_stream_init(s,read_input,seek_input,&r);int16_t pcm[514];unsigned frames=0;int rc;
    while((rc=mp3_stream_read(s,pcm,257))>0){
        assert(rc<=257);frames+=(unsigned)rc;assert(frames<24000*10);
        if(output)assert(fwrite(pcm,4,(size_t)rc,output)==(size_t)rc);
    }
    if(rate)*rate=mp3_stream_rate(s);
    if(channels)*channels=mp3_stream_channels(s);
    free(s);return rc<0?rc:(int)frames;
}
int main(int argc,char **argv){
    assert(argc==5);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
    unsigned char *data=malloc(size+100);assert(data&&fread(data,1,size,f)==size);fclose(f);
    FILE *out=fopen(argv[2],"wb");assert(out);unsigned rate,channels;
    int frames=run(data,size,8192,out,&rate,&channels);fclose(out);
    if(frames<0)fprintf(stderr,"decode error %d for %s\n",frames,argv[1]);
    assert(frames>=12000 && frames<18000 && rate==(unsigned)atoi(argv[3]) && channels==(unsigned)atoi(argv[4]));
    assert(run(data,size,37,NULL,NULL,NULL)==frames);
    struct reader r={data,size,0,8192,0,1};mp3_stream_t *s=malloc(mp3_stream_bytes());assert(s);
    mp3_stream_init(s,read_input,seek_input,&r);int16_t pcm[1024];assert(mp3_stream_read(s,pcm,512)==ERR_IO);
    assert(mp3_stream_read(s,pcm,512)==ERR_IO); /* error is sticky */
    assert(mp3_stream_read(s,pcm,0)==ERR_INVAL);free(s);
    unsigned char bad[32]={ 'I','D','3',4,0,0,0x80,0,0,0 };
    assert(run(bad,sizeof bad,17,NULL,NULL,NULL)<0);
    memset(bad,0,sizeof bad);assert(run(bad,sizeof bad,17,NULL,NULL,NULL)<0);
    assert(run(bad,0,17,NULL,NULL,NULL)<0);
    unsigned seed=17;unsigned char noise[2048];
    for(unsigned round=0;round<50;round++){
        for(unsigned i=0;i<sizeof noise;i++){seed=seed*1664525u+1013904223u;noise[i]=seed>>24;}
        assert(run(noise,sizeof noise,137,NULL,NULL,NULL)<0);
    }
    free(data);printf("MP3 stream: %s -> %d frames @24kHz, short reads/errors/malformed data PASS\n",argv[1],frames);
}
