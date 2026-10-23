minimp3, upstream commit ea99364f61c14656440e8d77e9c233ccf3124633:
https://github.com/lieff/minimp3

`minimp3.h` is unmodified upstream source. The CC0 license is in LICENSE.
The MeOS wrapper enables MP3-only scalar decoding (no SIMD). Decoding is done
in a separate 32KB-stack native app task, never in the audio rendering callback.
ESP32-P4 context switches/ticks now preserve the single-precision floating-point
registers/FCSR used by this decoder. S3 playback is not enabled.
