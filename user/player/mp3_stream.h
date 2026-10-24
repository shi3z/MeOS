#pragma once
#include <m5os/types.h>
/* Decoder storage is caller-owned; read/seek callbacks never run in the audio task. */
typedef int (*mp3_read_fn)(void *ctx,void *buf,size_t len);
typedef int (*mp3_seek_fn)(void *ctx,uint32_t absolute);
typedef struct mp3_stream mp3_stream_t;
size_t mp3_stream_bytes(void);
void mp3_stream_init(mp3_stream_t *s,mp3_read_fn read,mp3_seek_fn seek,void *ctx);
/* 24kHz stereo output; mono is duplicated. Returns frames, 0 at EOF, or ERR_*. */
int mp3_stream_read(mp3_stream_t *s,int16_t *stereo,unsigned frames);
unsigned mp3_stream_rate(const mp3_stream_t *s);
unsigned mp3_stream_channels(const mp3_stream_t *s);
