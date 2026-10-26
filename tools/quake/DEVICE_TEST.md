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


## Integrated SDMMC / OTA qualification — 2026-10-25

Source f2c8d0e integrates the committed masanork SDMMC, SD font/dictionary,
A/B layout, Home and editor changes with the Quake optimization/profiling work.
The physical partition table and OTA sequences were read first: ota_1 was
selected. A private full 16 MiB backup was made before installing and hash-verifying
only ota_0 (0x410000), then selecting it. Factory recovery and ota_1 were retained;
no bootloader/partition-table replacement, card format or PAK upload was performed.
The new image is 1613920 bytes; exact hash and observations are in
`sdmmc-device-2026-10-25.json` (the JSON image length is authoritative).

Sound-enabled shareware E1M1 reached complete signon in 24,732 ms on the first
launch; successive PPA and CPU launches took 15,942 and 15,847 ms. Boot-to-shell
samples were 4.3–4.7 seconds. Loading/background activity differs between runs;
these are not cold-cache, identical-condition comparisons. Home was suppressed
using the shell-ready grace period for deterministic serial recovery.

The CPU starting view ran about 46 FPS. A 2-second forward input followed by
8 seconds of left-turn/fire produced intervals down to 25.1 FPS; a low-detail
wall view subsequently exceeded 60 FPS. Steady gameplay read no SD data. CPU
copy/flush reached about 17–18 ms/frame during motion, whereas game-task effect
mixing remained well below 1 ms. The cache reported no thrash transitions in
these intervals; the 600 KiB default was retained.

The PPA offscreen test passed twice, but its E1M1 starting view ran about 29 FPS,
and motion still included sub-30 FPS intervals. It is not a universal speedup.
The two input sequences were equal-duration serial scripts, not frame-identical
A/B runs, and cannot isolate the near-clamp change from the other integration
changes. CPU remains the default.

Normal exits after 2,143 CPU frames and 1,506 PPA frames restored keyboard text
routing and reported respectively 3,140 / 3,015 audio DMA blocks, zero reported
underruns/errors, and nonzero PCM output. All sampled stack guards were OK.
Internal heap usage settled at 92 KiB after both; the internal failure counter
increased once during the first session and then remained at one. Its origin is
untraced (internal-to-PSRAM fallbacks also increment it); no visible app/audio
failure occurred. PSRAM failure count stayed zero. PPA retained its expected
roughly 455 KiB reusable kernel buffers. A third CPU E1M1 launch reached gameplay
and was left for the user. Constant 30 FPS remains unqualified.


## CPU column transfer follow-up — 2026-10-25

`columns-device-2026-10-25.json` records two full-view 969-frame demo1 timedemos
per firmware, with effects enabled and CPU rendering. The old image returned
24.6 FPS twice; congruent-halfword peeling plus word damage scanning returned
24.8 twice; three-row broadcast copying returned 25.0 twice. One initial
console-visible run is explicitly excluded. This is about 1.6% reported gain,
not constant 30 FPS. Every candidate matches the independent host RGB reference.
Normal app exits reported zero audio DMA errors and restored keyboard routing;
all reported stack guards passed, with the 48 KiB stack request retained.
Only the selected Quake OTA slot was replaced; recovery/other-slot images and SD
data were retained. The final candidate was restarted into CPU E1M1 for the user.
A late-start internal-memory fragmentation failure remains separate from this
transfer change; its return code is now exposed without reducing stack capacity.

## Exact DMA2D / visual qualification — 2026-10-25

The opt-in `quake dma` prototype passed offscreen three-row copy/guard checks,
static and moving native full-pixel comparisons (zero mismatches), and user
confirmation of the comparison patterns and Quake's actual display/speed.
CPU and DMA static reference FNV32 was `986b6f69`; an independent host reference
generator produces the same value. The older noisy benchmark was not counted as
visual qualification after the user reported apparent corruption.

With effect audio, full-view demo1 returned 969 frames / 38.8 seconds / 25.0 FPS
for both CPU repeats, and 969 frames / 30.3 seconds / 32.0 FPS for four DMA repeats
(two for filming). Normal CPU/DMA app exits reported zero audio DMA errors and
sampled stack guards remained intact. No constant-30-FPS claim is made. The
measured image predates the final PR rebase onto newer network/storage commits;
its identity, counters and explicit remaining physical gates are recorded in
`dma-device-2026-10-25.json`. CPU remains the default path.

## 512x300 baseline — 2026-10-26

Source `f4e9ddb` was installed to OTA slot 0 only, preserving the factory image,
other OTA slot and SD data. CPU 360 MHz, 128 KiB L2, 600 KiB surface cache and
effect audio were held fixed. Full-view `timedemo demo1;toggleconsole` produced:

| Mode | Run 1 | Run 2 | Frames per run |
| --- | --- | --- | --- |
| CPU | 90.4 s / 10.7 FPS | 90.3 s / 10.7 FPS | 969 |
| Exact DMA | 168.7 s / 5.7 FPS | 168.7 s / 5.7 FPS | 969 |

Static CPU/DMA references match `a4258a41`; moving DMA has zero pixel
mismatches. Sampled stack guards pass; both measured-mode exits report zero
audio underruns/errors and normal app relaunch succeeds. Human visual/audio
and control acceptance at this resolution remains pending. The final app is
CPU E1M1 at 512x300, expanded to 1024x600 at (128,60).

The prior 320x200 DMA result was 32.0 FPS. This high-resolution adaptation
exposes a transfer bottleneck: moving DMA intervals spend about 130 ms/frame
in transfer, versus about 60 ms for CPU. Cache behavior of column traversal
with a 150 KiB source and 128 KiB L2 is a follow-up hypothesis, not an isolated
cause. See `hires-device-2026-10-26.json` for image identity and raw run values.

## High-resolution tiled transfers — 2026-10-26

The 16-column candidate (`2dff44f`) returned CPU 14.1 / 14.1 FPS and DMA
16.0 / 16.0 FPS. The selected 64-column candidate (`926ffec`) returned CPU
15.9 / 15.9 FPS (60.8 seconds each) and DMA 17.5 / 17.5 FPS (55.4 seconds each),
all full-view 969-frame demo1 with effect audio and baseline clock/cache/math.
Both candidate modes pass native static/moving pixel comparisons; the static
reference remains `a4258a41`. The user confirmed normal display on 16 columns and normal display/controls
on the final 64-column DMA game after relaunch.

The selected candidate peaks at 30,896 / 49,152 sampled stack bytes, guard OK;
normal CPU/DMA exits report zero audio underruns/errors. The internal-stack
tile grows to 19,200 bytes but is never passed to DMA. Existing persistent
SRAM transfer buffers and bounded DMA shutdown remain unchanged. A known
post-diagnostic internal-heap fragmentation startup failure required a reset
and early game start; normal measured game quit/relaunches then passed.
Both resolution builds and ASan/UBSan checks pass; low-resolution transfer
branches remain unchanged. See the two `hires-tile*-device-2026-10-26.json` files.

## Batched DMA — 2026-10-26

Compare latest baseline `7c343f6` with selected two-column candidate `333a366`:
969-frame full-view demo1, effect audio, CPU/cache/renderer settings unchanged.
Baseline: 55.3 / 55.3 seconds, 17.5 / 17.5 FPS. Selected: 52.7 / 52.7 seconds,
18.4 / 18.4 FPS. Low-resolution shared submission compatibility: 29.7 / 29.7
seconds, 32.6 / 32.6 FPS. Native static/moving CPU/DMA pixel comparisons pass;
selected audio reports nonzero PCM and zero underruns/errors on normal exit.
Sampled selected stack peak is 29,696 / 49,152 bytes, guards intact.

Verify the actual `audio: ES8388` startup message and positive nonzero PCM
output before accepting sound-enabled performance. The four-column prototype
failed audio allocation at stage 4; its silent 19.3 FPS is not comparable.
One two-column attempt with no result row is also excluded. Late diagnostic
exit can still fragment internal RAM and prevent the 48 KiB app stack from
starting; early game launch after reset was used. The selected 5,247-byte
source allocation is permanent kernel-owned internal memory, not app-owned.

Detailed evidence and incomplete qualification are recorded in
`dma-batch-device-2026-10-26.json`. Human display/audio acceptance remains
separate from automated pixel checks and PCM counters.

## Quiet renderer profile — 2026-10-26 (partial)

Quiet normal/profile images select DAC attenuation 191 (-95.5 dB), read back
both channels before amp enable, and retain effect mixing/DMA. Default gain
remains -18 dB. Full-view demo1 returns normal 18.5 / 18.5 FPS (52.3 / 52.4 s),
diagnostic 18.4 / 18.4 FPS (52.5 / 52.6 s), 969 frames each. Audio exits have
zero underruns/errors and positive PCM. Stack guards pass at sampled peaks
29,920 / 49,152 and 29,888 / 49,152 bytes.

Steady diagnostic interval medians: transfer 14.81 ms, world edges 28.72 ms
(inclusive), texture spans 11.84 ms, depth spans 6.00 ms, cache 3.75 ms
(includes build 3.37 ms, including lightmap 0.21 ms). Nested values must not
be added; all times include preemption/IRQs.

Ownership guards rejected DMA after USB-only resets with an existing module
clock; CPU fallback runs are excluded. An esptool watchdog reset restored a
claimable module, after which fresh normal game exits and USB resets retained
working DMA. Do not reset/take an actively owned module to force a benchmark.

Zpad32 was flashed, but USB enumeration disappeared before its first monitored
boot. Both Z-pitch performance comparisons, final display/control/listening
acceptance and pitch selection remain pending reconnection. The prepared
images remain quiet. Evidence: `quiet-profile-device-2026-10-26.json`.
