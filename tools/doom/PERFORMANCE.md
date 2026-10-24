# Tab5 Doom performance

Measured 2026-10-23 on the connected ESP32-P4 Tab5, CPU 360MHz, PSRAM 200MHz,
320x200 engine image displayed at 960x720, side controls and audio enabled.
Clock frequency and game simulation rate were not increased. These are rendered
frame rates, not LCD scanout rates or guarantees for every map/combat situation.
Exclude the first 100 frames (startup/wipe) from steady-state comparisons.

| Build stage | Measured FPS | Main observation |
| --- | --- | --- |
| RGB888 -> RGB565, general resized blit | 12.4–12.8 | 78–80ms/frame: display 49–50ms, color conversion 7ms |
| Indexed palette presentation, repeated-column copies, display -O2 | 22.1–22.9 | about 31ms/frame in display work |
| Eight-column SRAM tiles and packed 4:3 expansion | about 28 | about 26ms/frame in display work |
| Unchanged HUD preservation | 30.5–31.3 stationary; 27.4–28.9 with automated input | includes real changed game states, not duplicate frames |
| Final cycle-counted ST I2C build | 30.5–31.4 stationary over 50 seconds | 1,534 frames; 2,338 audio blocks; zero detected audio/touch errors |

The indexed path preserves palette changes and gamma through the original
engine I_SetPalette implementation, then builds a small RGB565 lookup table.
Only upstream i_video.c's I_SetPalette and I_FinishUpdate definitions are renamed
at compile time; vendored source stays unchanged. A MeOS adapter sends
I_VideoBuffer directly to the Tab5 renderer. The original RGB888 buffer allocated
by Doomgeneric is retained but not used for presentation.

The 320x200 fast path reads eight adjacent columns into a small SRAM tile,
expands five source rows to eighteen output rows (4+4+3+4+3) and writes repeated
columns in aligned words. Pixel mapping is identical to the general nearest
neighbor reference. The lower 32 source rows are redrawn only when their bytes
or the palette differ; otherwise the existing lower 115 display rows are kept.
The previous HUD cache is app-owned and reclaimed on exit.

Host comparison of the 120th E1M1 frame against the prior RGB path found zero
changed RGB565 channel values (192,000 channel values checked). ASan/UBSan tests
compare indexed and RGB output across the entire viewport, untouched side rails,
clipped/odd boundaries and HUD-preserving updates. The renderer tests also pass
with -O3 and loop unrolling, as used by the Doom display build.

The outer loop yields rather than sleeping unconditionally after each frame;
the engine's timing logic still enforces real-time simulation. Profiling splits
preparation, display copy, cache flush, touch acquisition and waiting. A second
counter records frames with distinct gametic values, so repeated presentation
of the same simulation state cannot masquerade as smoother gameplay. Startup
wipe frames are counted in presentation FPS but not as distinct game frames.
The original simulation is 35Hz. Smooth 60fps needs separate rendering and
interpolation work, not a faster simulation clock.

Touch report I2C timing is per transfer: ST712x status/points use a cycle-counted
>=1us half-period and retry at the original timing on NACK. GT911, boot probes
and other clients retain their 4us delays. There is no global timing switch to
leak across preemption or task termination. Host tests exercise both buses,
legacy delays, fast delays and NACK fallback. Manual fast-mode touch qualification
remains pending; the prior multi-touch controls were confirmed by the user.

Boot recovery releases a stuck SDA with nine clocks and STOP; expander resets
retry initial NACKs. Three consecutive hardware boots initialized the LCD.
Doom now refuses to start without a framebuffer and active scanout: an earlier
boot without an LCD produced meaningless >60 presentation counts, which were
excluded. A 45-second automated movement/use/fire run completed with zero audio
DMA errors, zero touch I2C errors, 49 sound effects and 10,789,552 bytes reclaimed.
Its PSRAM usage returned to the prior baseline and a subsequent launch succeeded.

The final cycle-counted I2C build completed 50 seconds, reclaimed 10,789,552
bytes, returned to the PSRAM baseline and relaunched. A later automated-input
check overlapped another session flashing non-Doom firmware onto the same Tab5.
Those later serial results (including command-not-found output) are excluded;
exclusive ownership of the physical device is needed for further qualification.
The final build's manual touch check remains pending.
Evidence logs live in the local MeOS-backups folder. No WADs or game screenshots
are committed.

After device access was released, the verified firmware (SHA256
c38764e5a7cb62a9ba1352bee17821ac04679d3ccaceabfe862449b8d6152277) was restored
without rebuilding concurrent source changes. Flash verification, LCD init,
ES8388 output and nonempty presentation passed. A steady-state sample measured
31.3 presentation fps and 31.3 distinct game frames/s; Doom was left running.
