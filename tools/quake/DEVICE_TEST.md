# Tab5 Quake physical qualification — 2026-10-24

MeOS remains shi3z's project. This records a local prototype, not a release or a
new project license. Game data and captured game images are not committed.

## Device and data

- ESP32-P4 Tab5, ST7121 display, ST712x touch, 360 MHz, ESP-IDF 5.4.4.
- 16 GB SDHC, 30,867,456 sectors / 15,072 MiB, FAT mounted **read-only**.
- Shareware v1.06 PAK0: 18,689,235 bytes. Fresh SD read returned CRC32 `74e3b084`,
  matching the host source. SHA256 is recorded in README.md. No format performed.
- Original application partition was backed up separately before experimentation.
  No bootloader, partition table, dictionary or application-store flash region was
  replaced. Local binary/symbol backups remain outside the repository.

## Passed

1. Full engine linked/booted with 12 MiB arena and 48 KiB internal task stack.
2. Demo1/e1m3 rendered on the device; an indexed frame/palette capture was decoded
   and visually inspected. This capture is not a photograph of the physical LCD.
3. Serial Escape/Enter selected Single Player / New Game. `maps/start.bsp` loaded,
   local client/server connected, and the player entered the Introduction map.
4. Forward input moved the viewpoint toward the hallway. Fire input reduced
   ammunition from 25 to 24; before/after device engine-frame captures confirmed
   movement and HUD changes. Audio is intentionally disabled.
5. Three normal launches and quits without resetting the board. Each completed
   app reported 13,517,504 bytes reclaimed; no live app tasks remained.
6. After each quit, coarse used memory returned to 112 KiB internal and 8,869 KiB
   PSRAM. Free PSRAM was 18,562 KiB. Largest contiguous free block varied (13,138
   then 12,621 KiB); this is not a fragmentation-free or long-campaign claim.
7. SD unmount succeeded after gameplay, followed by read-only mount and restart.
8. Samples reported 15,344 / 49,152 stack bytes used, guard OK. Other task guards
   remained OK after quitting; no allocation failures were reported.

Post-load 100-presentation intervals ranged roughly **11.8–17.4 presentations/s**
across the tested demo/Introduction scenes. Loading intervals (2.3 / 3.4) and
capture stalls are not steady-state results. These counts are display submissions,
not a guarantee of distinct pixel contents, a full-game benchmark or 30 FPS.

## Bring-up fixes and residual checks

The first image tripped the scheduler's saved-SP check. Additional diagnostics
identified the incoming Quake task, rather than the outgoing task named in the
panic record. The PAK parser's 128 KiB automatic directory array and other large
fixed buffers were moved into resettable PSRAM BSS for this single-engine task.
Host ASan rendering still completed 130 frames, with byte-identical captured PPM
before/after the storage-class changes. Engine GCC stack reports now top out at
9,792 bytes and are checked against a 16 KiB per-function ceiling. Runtime stack
checking is kept enabled; this static ceiling does not bound recursive call chains.

Physical touch hit targets/menu selection, long-play/other levels, forced task
termination, camera verification of LCD output and audio remain separate checks.
The final FIRE/OK context mapping is a follow-up to the serial Enter qualification;
it was also exercised through serial F to enter New Game, but physical finger
interaction must not be inferred from that test.

## Input revision and combined SD-root boot

The follow-up visible rail controls and raw keyboard lease were integrated with
`integration/tab5-sdroot` at d10b54b. The installed 16 GB card now contains the
littlefs/COW-backed M5FS root from that separate work, so the earlier FAT PAK
qualification does not apply to this card's current contents. No formatting was
performed during input qualification. A new app-partition backup was retained
outside the repository before replacing the installed image.

The combined input image booted the existing persistent SD root. Asset-free
`quake controls` started on the physical Tab5, logged `keyboard: game input
active` and `quake controls: keyboard ready`. Serial W produced separate key 119
down/up edges. Serial Ctrl-C exited normally and the production accessory driver
logged `keyboard: text input restored`. These observations qualify the actual
I2C mode handoff and serial path, not physical key presses, USB keyboards, finger
hit targets or gameplay with this new input revision. Host tests cover matrix
chords/releases, bounded event routing and normal HID/text restoration.


## Native SD bulk assets — completed

The same 16 GB littlefs/COW SD-root card now stores the separately supplied
shareware PAK at `/assets/quake/id1/pak0.pak`. No format, M5FS resize or root-store
replacement was performed. A dedicated littlefs-backed VFS directory avoids the
8 MiB M5FS capacity limit; internal base/delta files remain outside its namespace.

The successful USB upload took 1,528.9 seconds. Received size/CRC and an independent
full SD reread both returned 18,689,235 bytes / `74e3b084`, matching the source.
An earlier attempt was interrupted by macOS sleep; receive timeout removed the
incomplete file. Scoped `caffeinate` prevented a repeat. First-copy and full-file
verification performance remain slow; this is not a storage-speed improvement.

After a hardware reset (no new flash), the native root and `/assets` mounted,
`pak0.pak` retained its size, and `/home/sd-boot.txt` still read `qualification-099`.
Quake recognized 339 PAK entries from the native path, played its demo and entered
NEW GAME / `maps/start.bsp`. Post-load intervals reported 15.6 and 15.4 display
presentations/s, with 15,424 / 49,152 sampled stack bytes and guard OK. Real native
keyboard transitions reached 26 during gameplay; this does not qualify every key
or touch target. The device is left in the Introduction game for user operation.
Game assets, firmware and captures remain outside the repository. Exact summary:
`native-assets-device-2026-10-24.json`.

## FPS follow-up (CPU backend)

Profiling now separates engine frames, hardware LCD refresh, indexed copy/flush,
input and explicit waits. The ST7121 refresh counter measured about 65.4 Hz; the
70 MHz requested DPI clock is integer-divided by this SDK, so the request alone
is not an actual refresh measurement. The viewport remains 320x200 -> 960x720.

The original full-copy path spent about 28 ms in copying per frame. Removing
redundant yield/sleep improved the demo from about 12.6 to 16-18 engine FPS.
Speed-oriented LCD compilation alone left the copy near 25 ms. Exact per-column
source damage tracking reduced static-scene presentation to about 3 ms. Software
double floor division/depth scaling and vector square roots were replaced with
mathematically equivalent integer/FP32 operations, with scalar O3 compilation.
A million reference cases and a 130-frame ASan render (SHA256 unchanged) pass.
128-byte cache lines improved the starting-view intervals to 30.6-33.4 FPS.

A final 10-second serial camera-rotation test reported 30.5, 25.0 and 27.9 FPS;
the first interval includes the transition into motion. **Constant 30 FPS is not
qualified**. Earlier user-controlled intervals ranged about 23-50 FPS, dependent
on viewpoint and motion. These phase ranges are not camera-identical A/B runs.
Stack samples remained 11,584 / 49,152 with guard OK. Native keyboard events
continued, normal quits restored text mode, and Doom/simulator checks passed.

An optional PPA prototype passed two offscreen quadrant/orientation/guard tests
on the physical P4. Its revised ownership/cached-buffer implementation and game
FPS still need qualification; a concurrent atomic-editor device session took
the serial port before that final test. The CPU mode remains the default.
Exact CPU observations: `performance-device-2026-10-24.json`.


## PPA continuation and effect audio

The later PPA session at source c932851 passed five offscreen tests, 4,738 game
frames, keyboard routing, normal exit and reuse without PSRAM growth after warmup.
46 post-load intervals averaged 29.56 FPS harmonically (21.5–35.7 range).
Constant 30 FPS, forced termination, injected DMA faults and user visual acceptance
remain unqualified. See `ppa-device-2026-10-24.json`; this supersedes the earlier
pending game-performance note above.

The following audio-enabled image (SHA256 in `audio-device-2026-10-24.json`)
started the shareware `e1m1` / The Slipgate Complex map directly. The PPA ownership
check reported busy, so this session safely retained the classic CPU renderer.
The ES8388 initialized at 24 kHz; Quake mixed effects at 11025 Hz. Host conversion
and stall-silence tests, Quake build and separate 64-byte-cache-line Doom build pass.
A normal exit after 2,716 game frames reported 9,968 DMA blocks, zero reported
underruns/errors, 5,103,616 output frames and 766,576 nonzero output frames. Audio
status returned idle, native keyboard text routing returned, and allocation failure
counters were zero. The sampled game stack reached 13,088 / 49,152 bytes, guard OK.
The user subsequently confirmed audible effect output from the speaker. CD/BGM support remains disabled. A second E1M1
launch reached gameplay after cleanup (44.1–44.2 FPS sampled at the starting view). No SD format, data upload or engine registration
check bypass was performed.
