#include "mp3_stream.h"
#include <m5os/errno.h>
#include <string.h>
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "../../third_party/minimp3/minimp3.h"
#define INPUT_BYTES 8192u
#define OUTPUT_RATE 24000u
struct mp3_stream {
    mp3dec_t decoder;
    mp3_read_fn read;mp3_seek_fn seek;void *ctx;
    uint8_t input[INPUT_BYTES];
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    unsigned bytes,samples,index,rate,channels,phase,junk;
    bool initialized,eof,finished,primed;
    int error;
    int16_t left[2],right[2];
};
size_t mp3_stream_bytes(void){return sizeof(mp3_stream_t);}
void mp3_stream_init(mp3_stream_t *s,mp3_read_fn read,mp3_seek_fn seek,void *ctx){
    memset(s,0,sizeof *s);s->read=read;s->seek=seek;s->ctx=ctx;mp3dec_init(&s->decoder);
}
static int refill(mp3_stream_t *s){
    if(s->eof || s->bytes==INPUT_BYTES)return ERR_OK;
    int n=s->read(s->ctx,s->input+s->bytes,INPUT_BYTES-s->bytes);
    if(n<0)return n;
    if((unsigned)n>INPUT_BYTES-s->bytes)return ERR_IO;
    s->bytes+=(unsigned)n;if(!n)s->eof=true;return ERR_OK;
}
static int begin(mp3_stream_t *s){
    while(s->bytes<10 && !s->eof){int rc=refill(s);if(rc<0)return rc;}
    if(s->bytes>=10 && !memcmp(s->input,"ID3",3)){
        uint8_t *h=s->input;
        if(h[3]<2 || h[3]>4 || (h[6]|h[7]|h[8]|h[9])&0x80)return ERR_CORRUPT;
        uint32_t size=((uint32_t)h[6]<<21)|((uint32_t)h[7]<<14)|((uint32_t)h[8]<<7)|h[9];
        if(size>16u*1024*1024)return ERR_CORRUPT;
        uint32_t offset=10+size+((h[3]==4 && (h[5]&0x10))?10:0);
        if(!s->seek || s->seek(s->ctx,offset)!=(int)offset)return ERR_CORRUPT;
        s->bytes=0;s->eof=false;
    }
    s->initialized=true;return ERR_OK;
}
static int decode(mp3_stream_t *s){
    for(;;){
        if(!s->initialized){int rc=begin(s);if(rc<0)return rc;}
        while(s->bytes<4096 && !s->eof){int rc=refill(s);if(rc<0)return rc;}
        if(!s->bytes)return s->rate && !s->junk?0:ERR_CORRUPT;
        if(s->eof && s->bytes==128 && !memcmp(s->input,"TAG",3)){s->bytes=0;return s->rate?0:ERR_CORRUPT;}
        if(s->eof && s->rate){
            bool zeros=true;for(unsigned i=0;i<s->bytes;i++)if(s->input[i])zeros=false;
            if(zeros){s->bytes=0;return 0;}
        }
        mp3dec_frame_info_t info;
        int n=mp3dec_decode_frame(&s->decoder,s->input,(int)s->bytes,s->pcm,&info);
        unsigned consumed=info.frame_bytes>0?(unsigned)info.frame_bytes:0;
        if(consumed>s->bytes)return ERR_CORRUPT;
        if(!consumed){
            if(!s->eof && s->bytes<INPUT_BYTES){int rc=refill(s);if(rc<0)return rc;continue;}
            return ERR_CORRUPT;
        }
        s->bytes-=consumed;memmove(s->input,s->input+consumed,s->bytes);
        if(n>0){
            if(info.layer!=3 || (info.channels!=1 && info.channels!=2) || info.hz<8000 || info.hz>48000 || n>1152)return ERR_CORRUPT;
            if(s->rate && (s->rate!=(unsigned)info.hz || s->channels!=(unsigned)info.channels))return ERR_NOSYS;
            s->rate=info.hz;s->channels=info.channels;s->samples=n;s->index=0;s->junk=0;return 1;
        }
        s->junk+=consumed;if(s->junk>65536)return ERR_CORRUPT;
    }
}
static int source(mp3_stream_t *s,int16_t out[2]){
    if(s->index==s->samples){int rc=decode(s);if(rc<=0)return rc;}
    out[0]=s->pcm[s->index*s->channels];
    out[1]=s->pcm[s->index*s->channels+(s->channels==2)];s->index++;return 1;
}
int mp3_stream_read(mp3_stream_t *s,int16_t *out,unsigned frames){
    if(!s || !s->read || !out || !frames || frames>4096)return ERR_INVAL;
    if(s->error)return s->error;
    if(s->finished)return 0;
    if(!s->primed){
        int rc=source(s,s->left);if(rc<=0){s->error=rc<0?rc:ERR_CORRUPT;return s->error;}
        rc=source(s,s->right);if(rc<=0){s->error=rc<0?rc:ERR_CORRUPT;return s->error;}
        s->primed=true;
    }
    unsigned n=0;
    while(n<frames && !s->finished){
        for(unsigned ch=0;ch<2;ch++)out[n*2+ch]=(int16_t)(s->left[ch]+
            ((int32_t)s->right[ch]-s->left[ch])*(int32_t)s->phase/(int32_t)OUTPUT_RATE);
        n++;s->phase+=s->rate;
        while(s->phase>=OUTPUT_RATE){
            s->phase-=OUTPUT_RATE;s->left[0]=s->right[0];s->left[1]=s->right[1];
            int rc=source(s,s->right);
            if(rc<=0){s->finished=true;if(rc<0)s->error=rc;break;}
        }
    }
    return (int)n;
}
unsigned mp3_stream_rate(const mp3_stream_t *s){return s->rate;}
unsigned mp3_stream_channels(const mp3_stream_t *s){return s->channels;}
