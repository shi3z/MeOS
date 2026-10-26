/* Quake software mixer -> MeOS audio ring. The callback only copies integer PCM:
 * no engine calls, allocation, VFS access or floating point on the audio task. */
#include "quakedef.h"
#include "audio_ring.h"
#include <m5os/audio.h>
#include <m5os/arch.h>
#include <m5os/kprintf.h>
extern uint32_t quake_owner;
int snd_inited;
static dma_t device;
static quake_audio_state stream;
static bool opened;
#define MONO_SAMPLES 16384u
static void render(int16_t *out,unsigned frames,void *arg){
    (void)arg;quake_audio_render((int16_t *)device.buffer,MONO_SAMPLES,&stream,out,frames,11025,AUDIO_RATE);
}
qboolean SNDDMA_Init(void){
    memset(&device,0,sizeof device);memset(&stream,0,sizeof stream);opened=false;
    device.samplebits=16;device.speed=11025;device.channels=2;
    device.samples=MONO_SAMPLES;device.submission_chunk=1;
    device.buffer=Hunk_AllocName(MONO_SAMPLES*sizeof(int16_t),"meos-sound");
    if(!device.buffer)return false;
    memset(device.buffer,0,MONO_SAMPLES*sizeof(int16_t));
    shm=&device;device.soundalive=device.gamealive=true;
    opened=audio_open(quake_owner,render,NULL);snd_inited=opened;
    if(!opened){device.soundalive=device.gamealive=false;kputs("quake: audio unavailable; continuing silent\n");}
    return opened;
}
int SNDDMA_GetDMAPos(void){return (int)__atomic_load_n(&stream.cursor,__ATOMIC_ACQUIRE);}
int SNDDMA_GetSamples(void){return MONO_SAMPLES;}
void quake_sound_cleanup(void){
    bool was_open=opened;audio_close(quake_owner);opened=false;snd_inited=0;
    if(was_open)kprintf("quake audio: %u output frames, %u nonzero frames\n",stream.frames,stream.nonzero);
}
void SNDDMA_Shutdown(void){quake_sound_cleanup();}
void SNDDMA_Submit(void){}
/* Music tracks are separate CD audio, not part of the effect WAVs in PAK0. */
int CDAudio_Init(void){return -1;}
void CDAudio_Play(byte t,qboolean loop){(void)t;(void)loop;}
void CDAudio_Stop(void){}
void CDAudio_Pause(void){}
void CDAudio_Resume(void){}
void CDAudio_Shutdown(void){}
void CDAudio_Update(void){}
