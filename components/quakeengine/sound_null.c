#include "quakedef.h"
int snd_inited;
qboolean SNDDMA_Init(void) { return false; }
int SNDDMA_GetDMAPos(void) { return 0; }
int SNDDMA_GetSamples(void) { return 0; }
void SNDDMA_Shutdown(void) {}
void SNDDMA_Submit(void) {}
int CDAudio_Init(void) { return -1; }
void CDAudio_Play(byte t,qboolean loop) { (void)t;(void)loop; }
void CDAudio_Stop(void) {}
void CDAudio_Pause(void) {}
void CDAudio_Resume(void) {}
void CDAudio_Shutdown(void) {}
void CDAudio_Update(void) {}
