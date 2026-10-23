# Tab5 audio output

`audio test` plays a 1.2-second, low-volume triangle tone. `audio` reports
DMA block and error counters. ES8388 gain starts at -18dB; the microphone ADC
is off. Output is 24kHz, 16-bit stereo on I2S0 and AHB GDMA TX2.

The driver uses a kernel worker and eight 512-frame buffers in internal SRAM.
Descriptors occupy separate 64-byte cache lines; ownership is invalidated
before reading and flushed on return to DMA. PCM data is flushed too.
No FreeRTOS APIs are used. The worker retains its 4KB stack after playback;
PCM buffers are freed on close. The first caller needs APP_PERM_SPAWN.

A task mutex serializes rendering and close. Callbacks must not block or call
`audio_close`. Application owners must pass their actual app ID; app teardown
stops audio before reclaiming owner memory. Close mutes the codec while clocks
are running, then stops DMA and frees buffers. Concurrent opens are rejected.
The codec and amp use fixed system-I2C helpers so accessory bus selection does
not redirect audio writes. One mutex covers each masked read-modify-write.

This PR excludes Doom integration and synthesis. Host I2C regressions cover
fixed-bus writes, atomic masked updates, routing and NACK handling. The actual
I2S/DMA/cache path and lifecycle changes require physical-device verification.
