#include <m5os/board_config.h>
#if M5OS_DOOM
/* MeOS implementation of Doom's public sound interface. Upstream i_sound.c is
 * replaced rather than enabling its SDL include path. Hardware callbacks never
 * call zone allocation or WAD APIs; samples are pinned by the game task first. */
#include <m5os/audio.h>
#include <m5os/arch.h>
#include <m5os/kprintf.h>
#include "runtime.h"
#include "mixer.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_config.h"
#include "w_wad.h"
#include "z_zone.h"
#include <string.h>
int snd_samplerate=AUDIO_RATE, snd_cachesize=2*1024*1024, snd_maxslicetime_ms=22;
int snd_musicdevice=SNDDEVICE_SB, snd_sfxdevice=SNDDEVICE_SB;
char *snd_musiccmd="";
static bool s_enabled, s_prefix, s_music_enabled;
static unsigned s_effects;
void doom_sound_reset(void) { doom_mix_reset(); s_enabled=s_prefix=s_music_enabled=false; s_effects=0; }
void doom_sound_shutdown(void)
{
    audio_close(doom_owner);
    if (s_enabled) kprintf("doom: %u sound effects started\n",s_effects);
    s_enabled=false;
}
/* Tab5 has one speaker. Preserve both channels' content rather than losing
 * hard-panned instruments/effects on the codec output feeding the amplifier. */
static void speaker_render(int16_t *pcm, unsigned frames, void *arg)
{
    doom_mix_render(pcm,frames,arg);
    for (unsigned i=0;i<frames;i++) {
        int16_t mono=(int16_t)(((int)pcm[i*2]+pcm[i*2+1])/2);
        pcm[i*2]=pcm[i*2+1]=mono;
    }
}
void I_InitSound(boolean prefix)
{
    s_prefix=prefix;
    bool disabled=M_CheckParm("-nosound")>0;
    s_music_enabled=!disabled && M_CheckParm("-nomusic")==0;
    if (!disabled) s_enabled=audio_open(doom_owner,speaker_render,NULL);
    if (!s_enabled) { s_music_enabled=false; kputs(disabled ? "doom: silent mode\n" : "doom: audio unavailable; continuing silent\n"); }
}
void I_ShutdownSound(void) { doom_sound_shutdown(); }
int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[9]={0}; unsigned off=s_prefix?2:0;
    if (off) { name[0]='d'; name[1]='s'; }
    for (unsigned i=0;i<8-off && sfx->name[i];i++) name[off+i]=sfx->name[i];
    return W_GetNumForName(name);
}
int I_StartSound(sfxinfo_t *sfx,int channel,int vol,int sep)
{
    if (!s_enabled || M_CheckParm("-nosfx") || channel<0 || channel>=DOOM_SFX_CHANNELS) return -1;
    int lump=sfx->lumpnum;
    const uint8_t *data=W_CacheLumpNum(lump,PU_STATIC);
    int len=W_LumpLength(lump);
    uint32_t irq=arch_irq_disable();
    bool ok=doom_mix_sfx_start(channel,data,len,vol,sep);
    arch_irq_restore(irq); if (ok) s_effects++; return ok?channel:-1;
}
void I_StopSound(int ch) { uint32_t irq=arch_irq_disable(); doom_mix_sfx_stop(ch); arch_irq_restore(irq); }
boolean I_SoundIsPlaying(int ch) { return s_enabled && doom_mix_sfx_playing(ch); }
void I_UpdateSoundParams(int ch,int vol,int sep)
{ uint32_t irq=arch_irq_disable(); doom_mix_sfx_params(ch,vol,sep); arch_irq_restore(irq); }
void I_UpdateSound(void) { }
void I_PrecacheSounds(sfxinfo_t *s,int n) { (void)s;(void)n; }
void I_InitMusic(void) { }
void I_ShutdownMusic(void) { I_StopSong(); }
void I_SetMusicVolume(int v)
{ uint32_t irq=arch_irq_disable(); doom_mix_music_volume(v); arch_irq_restore(irq); }
void I_PauseSong(void) { uint32_t irq=arch_irq_disable(); doom_mix_music_pause(true); arch_irq_restore(irq); }
void I_ResumeSong(void) { uint32_t irq=arch_irq_disable(); doom_mix_music_pause(false); arch_irq_restore(irq); }
static int s_song;
void *I_RegisterSong(void *data,int len)
{
    if (!s_enabled || !s_music_enabled || len<0) return NULL;
    uint32_t irq=arch_irq_disable(); bool ok=doom_mix_music_register(data,len); arch_irq_restore(irq);
    if (ok) kputs("doom: MUS music registered (integer synth, 16 voices)\n");
    return ok?&s_song:NULL;
}
void I_UnRegisterSong(void *h) { (void)h; I_StopSong(); }
void I_PlaySong(void *h,boolean loop)
{ if (!h) return; uint32_t irq=arch_irq_disable(); doom_mix_music_play(loop); arch_irq_restore(irq); }
void I_StopSong(void) { uint32_t irq=arch_irq_disable(); doom_mix_music_stop(); arch_irq_restore(irq); }
boolean I_MusicIsPlaying(void) { return s_enabled && doom_mix_music_playing(); }
void I_BindSoundVariables(void)
{
    M_BindVariable("snd_musicdevice",&snd_musicdevice); M_BindVariable("snd_sfxdevice",&snd_sfxdevice);
    M_BindVariable("snd_samplerate",&snd_samplerate); M_BindVariable("snd_cachesize",&snd_cachesize);
    M_BindVariable("snd_maxslicetime_ms",&snd_maxslicetime_ms); M_BindVariable("snd_musiccmd",&snd_musiccmd);
}
#endif
