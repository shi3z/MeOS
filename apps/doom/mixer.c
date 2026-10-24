/* Integer-only DMX mixer + small MUS synthesizer; no floating point, soundfont,
 * filesystem or allocator in the real-time path. Synthesized timbres are not OPL. */
#include <m5os/board_config.h>
#if M5OS_DOOM
#include "mixer.h"
#include <m5os/audio.h>
#include <string.h>
#include "music_tables.h"
#define VOICES 16
typedef struct { const uint8_t *data; uint32_t length, step; uint64_t phase; int left, right; } sfx_t;
typedef struct {
    bool active, released; uint8_t channel, note, velocity, program;
    uint32_t phase, step, age; int envelope;
} voice_t;
typedef struct {
    const uint8_t *data; size_t start, end, pos;
    uint32_t delay, clock, noise;
    bool playing, paused, loop;
    int volume;
    uint8_t velocity[16], program[16], gain[16], pan[16], bend[16], sustain[16];
    voice_t voices[VOICES]; unsigned steal;
} music_t;
static sfx_t s_sfx[DOOM_SFX_CHANNELS];
static music_t s_music;
static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p+2) << 16); }
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
void doom_mix_reset(void)
{
    memset(s_sfx, 0, sizeof s_sfx); memset(&s_music, 0, sizeof s_music);
    s_music.volume = 80; s_music.noise = 1;
}
void doom_mix_sfx_params(unsigned ch, int volume, int separation)
{
    if (ch >= DOOM_SFX_CHANNELS) return;
    volume = clamp(volume, 0, 127); separation = clamp(separation, 0, 254);
    s_sfx[ch].left = volume * (254-separation) / 127;
    s_sfx[ch].right = volume * separation / 127;
}
bool doom_mix_sfx_start(unsigned ch, const uint8_t *dmx, size_t len, int vol, int sep)
{
    if (ch >= DOOM_SFX_CHANNELS) return false;
    s_sfx[ch].data = NULL;
    if (!dmx || len < 8 || le16(dmx) != 3) return false;
    uint32_t rate = le16(dmx+2), n = le32(dmx+4);
    if (rate < 4000 || rate > 48000 || n > len-8 || n <= 32) return false;
    /* DMX includes sixteen padding bytes on each side of the actual PCM. */
    s_sfx[ch].length = n-32; s_sfx[ch].phase = 0;
    s_sfx[ch].step = (rate << 16) / AUDIO_RATE;
    doom_mix_sfx_params(ch, vol, sep); s_sfx[ch].data = dmx+24; return true;
}
void doom_mix_sfx_stop(unsigned ch) { if (ch < DOOM_SFX_CHANNELS) s_sfx[ch].data = NULL; }
bool doom_mix_sfx_playing(unsigned ch) { return ch < DOOM_SFX_CHANNELS && s_sfx[ch].data; }
void doom_mix_music_stop(void)
{ s_music.playing = false; memset(s_music.voices, 0, sizeof s_music.voices); }
static void defaults(void)
{
    memset(s_music.velocity, 100, 16); memset(s_music.gain, 100, 16);
    memset(s_music.pan, 64, 16); memset(s_music.bend, 128, 16);
    memset(s_music.program, 0, 16); memset(s_music.sustain, 0, 16);
}
bool doom_mix_music_register(const uint8_t *mus, size_t len)
{
    doom_mix_music_stop(); s_music.data = NULL;
    if (!mus || len < 16 || memcmp(mus, "MUS\x1a", 4)) return false;
    unsigned start = le16(mus+6), length = le16(mus+4), instruments = le16(mus+12);
    if (le16(mus+8) > 15 || start < 16u+instruments*2u || start > len || !length || length > len-start) return false;
    s_music.data = mus; s_music.start = start; s_music.end = start+length; return true;
}
void doom_mix_music_play(bool loop)
{
    if (!s_music.data) return;
    defaults(); memset(s_music.voices, 0, sizeof s_music.voices);
    s_music.pos = s_music.start; s_music.delay = s_music.clock = 0;
    s_music.loop = loop; s_music.paused = false; s_music.playing = true;
}
void doom_mix_music_pause(bool paused) { s_music.paused = paused; }
void doom_mix_music_volume(int v) { s_music.volume = clamp(v, 0, 127); }
bool doom_mix_music_playing(void) { return s_music.playing; }
static int byte(void)
{ return s_music.pos < s_music.end ? s_music.data[s_music.pos++] : -1; }
static void bend_voice(voice_t *v)
{
    int scale = 65536 + ((int)s_music.bend[v->channel]-128)*60;
    v->step = (uint32_t)(((uint64_t)steps[v->note] * (unsigned)scale) >> 16);
}
static void note_on(unsigned ch, unsigned note, unsigned velocity)
{
    if (!velocity) return;
    voice_t *v = NULL;
    for (unsigned i=0;i<VOICES;i++) if (!s_music.voices[i].active) { v=&s_music.voices[i]; break; }
    if (!v) v = &s_music.voices[s_music.steal++ % VOICES];
    *v = (voice_t){ .active=true, .channel=ch, .note=note, .velocity=velocity, .program=s_music.program[ch] };
    bend_voice(v);
}
static void note_off(unsigned ch, unsigned note)
{
    for (unsigned i=0;i<VOICES;i++) {
        voice_t *v=&s_music.voices[i];
        if (v->channel == ch && v->note == note) v->released = true;
    }
}
static void silence(unsigned ch)
{
    for (unsigned i=0;i<VOICES;i++) if (s_music.voices[i].channel == ch) s_music.voices[i].active=false;
}
static bool events(void)
{
    /* Invalid/zero-time streams must never monopolize the audio task. */
    for (unsigned limit=0;limit<512;limit++) {
        int ev=byte(), a, b; if (ev < 0) return false;
        unsigned ch=ev & 15, type=(ev >> 4)&7;
        if (type == 6) {
            if (!s_music.loop) return false;
            defaults(); memset(s_music.voices,0,sizeof s_music.voices);
            s_music.pos=s_music.start; s_music.delay=1; return true;
        }
        a=byte(); if (a < 0) return false;
        switch (type) {
        case 0: if (a > 127) return false; note_off(ch,a); break;
        case 1:
            if (a & 128) { b=byte(); if (b<0 || b>127) return false; s_music.velocity[ch]=b; }
            note_on(ch,a&127,s_music.velocity[ch]); break;
        case 2:
            s_music.bend[ch]=a;
            for (unsigned i=0;i<VOICES;i++) if (s_music.voices[i].active && s_music.voices[i].channel==ch) bend_voice(&s_music.voices[i]);
            break;
        case 3:
            if (a==10 || a==11) silence(ch);
            else if (a==14) { s_music.gain[ch]=100; s_music.pan[ch]=64; s_music.sustain[ch]=0; }
            else if (a<10 || a>14) return false;
            break;
        case 4:
            b=byte(); if (b<0 || b>127 || a>9) return false;
            if (a==0) s_music.program[ch]=b;
            if (a==3 || a==5) s_music.gain[ch]=b;
            if (a==4) s_music.pan[ch]=b;
            if (a==8) s_music.sustain[ch]=b;
            break;
        default: return false;
        }
        if (ev & 128) {
            uint32_t delay=0;
            for (unsigned n=0;;n++) {
                b=byte(); if (b<0 || n>=4) return false;
                delay=(delay<<7)|(b&127); if (!(b&128)) break;
            }
            s_music.delay=delay;
            if (delay) return true;
        }
    }
    return false;
}
static void music_sample(int *left, int *right)
{
    if (!s_music.playing || s_music.paused) return;
    if (!s_music.delay && !events()) { doom_mix_music_stop(); return; }
    s_music.clock+=140;
    if (s_music.clock>=AUDIO_RATE) { s_music.clock-=AUDIO_RATE; if (s_music.delay) s_music.delay--; }
    for (unsigned i=0;i<VOICES;i++) {
        voice_t *v=&s_music.voices[i]; if (!v->active) continue;
        unsigned ch=v->channel, family=v->program/8;
        v->age++; v->phase+=v->step;
        if (ch==15) {
            s_music.noise=s_music.noise*1664525u+1013904223u;
            if (v->age > AUDIO_RATE/4) { v->active=false; continue; }
            v->envelope=(int)((AUDIO_RATE/4-v->age)*32767u/(AUDIO_RATE/4));
        } else if (v->released && s_music.sustain[ch]<64) {
            v->envelope-=16; if (v->envelope<=0) { v->active=false; continue; }
        } else if (v->age<128) v->envelope=(int)v->age*255;
        else if (family<=1 || family==3 || family==4) {
            if (v->envelope>2000) v->envelope-=2;
        } else v->envelope=30000;
        int wave=sine[v->phase>>24];
        if (ch==15) wave=(int)(s_music.noise>>16)-32768;
        else if (family==3 || family==4) wave=(wave*3 + sine[(v->phase>>23)&255])/4;
        else if (family==2) wave=(wave + sine[(v->phase>>23)&255]/2)*2/3;
        else if (family==7 || family==10) wave=(wave*3+((v->phase&0x80000000u)?12000:-12000))/4;
        int sample=wave*v->envelope/32768;
        sample=sample*v->velocity/127; sample=sample*s_music.gain[ch]/127;
        sample=sample*s_music.volume/(127*8);
        *left += sample*(127-s_music.pan[ch])/127;
        *right += sample*s_music.pan[ch]/127;
    }
}
void doom_mix_render(int16_t *out, unsigned frames, void *arg)
{
    (void)arg;
    for (unsigned n=0;n<frames;n++) {
        int left=0,right=0;
        for (unsigned ch=0;ch<DOOM_SFX_CHANNELS;ch++) {
            sfx_t *s=&s_sfx[ch]; if (!s->data) continue;
            uint32_t pos=(uint32_t)(s->phase>>16);
            if (pos>=s->length) { s->data=NULL; continue; }
            int value=(int)s->data[pos]-128;
            if (pos+1<s->length) value += ((int)s->data[pos+1]-(int)s->data[pos])*(int)(s->phase&65535)/65536;
            left+=value*s->left/2; right+=value*s->right/2;
            s->phase+=s->step;
        }
        music_sample(&left,&right);
        out[n*2]=(int16_t)clamp(left,-32768,32767);
        out[n*2+1]=(int16_t)clamp(right,-32768,32767);
    }
}
#endif
