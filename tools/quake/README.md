# Experimental Quake on MeOS / Tab5

Worktree: `experiment/quake`, based on the extracted Doom/application/hardware
PR stack. MeOS is shi3z's work; no root license is assigned here. This prototype
is local only. No PAK, game assets or firmware release is included.

## Current milestone

- The Quake 1 software-renderer engine and MeOS adapters link for ESP32-P4 using
  ESP-IDF 5.4.4 and the existing RISC-V toolchain.
- 320x200 indexed rendering expands to the 960x720 center viewport; rails carry
  visible touch controls. Keyboard WASD moves/strafes, arrows turn/move,
  Ctrl or F fires, Space jumps, E changes weapon, Enter selects, Escape opens
  the menu, and Ctrl-C stops the app. Q remains available for game text input.
- Quake uses a separate image, not a concurrent Doom/Quake configuration. The
  default MeOS build remains Quake-disabled.
- The engine has a 12 MiB PSRAM arena. Large renderer scratch arrays move to
  resettable external BSS, leaving a 48 KiB internal task-stack request.
  Availability and measured stack usage still require device evidence.
- Private read-only stdio routes through the MeOS VFS, including random seeks.
  `/sd` data is streamed rather than loading an entire PAK into RAM. Handles,
  owner allocations and display/input state are reclaimed through app cleanup.
- Native firmware now includes the software effect mixer; its new audio backend
  passes host tests and physical mixer/DMA/normal-exit checks. Audible effect output
  was also confirmed by the user. CD music,
  network drivers, recording, config/save writes and savegame loading remain
  disabled. Only local loopback client/server is compiled.

## Build / checks

```
sh tools/quake/host_test.sh
sh tools/quake/build.sh
```

Outputs are `build-quake` and `sdkconfig.quake`, separate from Doom. The host
ASan/UBSan checks cover private file handles, read-only paths/modes, seek,
EOF/errors, handle exhaustion/cleanup, allocator and floating formatting.
The default checks do not run the full engine or exercise hardware.
Optional full-engine ASan smoke with separately supplied data:

```
python3 tools/quake/headless.py /path/to/pak0.pak --image /tmp/quake.ppm
```

The shareware v1.06 PAK (18,689,235 bytes, SHA256
`35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af`)
was tested locally through 130 software-renderer frames and a captured stage image.
This found and fixed a missing `getc` redirection in demo playback. Engine-marked
external-BSS variables are also folded into the resettable engine BSS so repeat
launches restore all state. The frame test is host evidence, not physical evidence. The engine retains legacy
parsers and assumes compatible trusted game data; this is not a hardened loader.

Local preparation on 2026-10-24: adapter/transfer tests, a complete real-engine
ASan host render (130 frames), the Tab5 firmware build, and a separate WAD-free
Doom regression build pass. The Quake image boots on the physical Tab5. A shareware
PAK was transferred to the original SDSC card with matching incoming-stream CRC
and a clean unmount. A FAT32 16 GB SDHC card was separately prepared through the
Mac reader with matching SHA256 and safe eject. Physical gameplay and normal quit/restart are now qualified as recorded in
`DEVICE_TEST.md`. Forced termination and physical touch interaction remain
separate qualification items.
Do not transfer historical Doom performance claims to this port.

## Next device check

Provide compatible Quake data separately, with `/id1/pak0.pak` on a FAT microSD
card (registered data may also have `pak1.pak`). Do not commit or redistribute PAK
files here. With the experimental firmware, use `sd mount ro`, then `quake`.
The new adapter always opens read-only, irrespective of the mount mode.
Back up the installed image before a coordinated device flash; another session
may be using the same device. First check PSRAM/stack, engine startup, first
frame, touch/keyboard input, quit, second launch and forced-stop cleanup. Measure
changed game frames separately from display presentations before optimization.

## Provenance

Sources are from Espressif's `esp32-quake` engine snapshot, pinned in
`third_party/quakegeneric/README.meos.md`. This in turn credits quake-generic and
id Software. Preserve GPL-2.0-or-later headers and LICENSE. Local engine changes
are documented there: dimensions and single-instance renderer scratch placement.
The Espressif FreeRTOS/BSP front end is not linked; MeOS supplies the runtime.

Primary reference: https://github.com/espressif/esp32-quake . That project reports
20–25 FPS at 512x300 on its P4 board; this is useful feasibility evidence, not a
Tab5/MeOS benchmark. Project-license and combined-work questions remain unresolved
before any binary release, as with the Doom experiment.

## Preparing a card

Use FAT32. Prefer copying `id1/pak0.pak` directly with a host card reader and
verifying its hash, then safely eject before inserting in Tab5. The 16 GB SDHC
card attached to the Mac was already FAT32; no format was performed.

An explicit developer transfer is available when a host card reader is absent:

```
python tools/quake/push_pak.py PORT /path/to/pak0.pak --prepare-sd --readonly-after
```

This uses `quake receive BYTES CRC32hex`, refuses an existing fixed destination,
and bounds size, times out, verifies received CRC and deletes a failed new file.
It writes only in this explicit preparation mode; the engine adapter stays read-only.
A serial link and synchronous 4 KiB VFS writes are substantially slower than a host
card reader. Default host tests cover overwrite refusal, two-block transfer,
CRC failure, timeout, write/close failure and cleanup. `quake verify` separately
reads the card back and reports size/CRC; receiver CRC alone verifies the incoming
stream rather than a fresh storage read. Use a known source CRC for comparison.

Press P during play to capture a diagnostic indexed engine frame to serial:

```
python3 tools/quake/frame_dump.py DEVICE_LOG /tmp/quake.png
```

The capture is the engine frame/palette, not a camera image of the physical LCD.
The profiling interval begins after Host_Init and reports presentations/second;
it does not claim every image contains changed scene content. Stack paint/guard
samples are diagnostics rather than exact maximum-depth proofs.

The physical bring-up caught legacy 128 KiB PAK-directory and up-to-88 KiB
renderer frames. Those fixed work arrays now live in resettable PSRAM BSS.
`check_stack.py` fails a firmware build if a reported engine frame exceeds
16 KiB or is unbounded; this is not a call-graph/recursion proof. Scheduler
bounds failures now name the rejected task, which can differ from the currently
running task stored by the panic recorder. Stack guards remain enabled.

Touch rails place the movement cross on the left and turn, JUMP and FIRE on
the right. MENU/WEAPON and EXIT/OK are at the top. Movement buttons navigate
menus; FIRE and OK select. Drag inside the viewport to look around.

Tab5 keyboard gameplay temporarily uses the official Normal matrix-event mode
for independent key presses/releases, including simultaneous keys and modifiers.
USB boot keyboards provide equivalent raw reports. The app releases ownership
on cleanup and the Tab5 driver restores HID text mode for the shell/IME. Queue
errors or overflow clear held keys; release and press again to resume. Serial
input remains supported with ANSI arrows and bounded holds; its single Escape
uses a short sequence timeout. Physical keyboards use actual release events.
The debug frame capture shortcut is serial P.

Use `quake controls` for an asset-free physical input/layout preview. It uses the
same input routing, shows the latest key edge, and exits via EXIT or Ctrl-C.

## Game data on the native SD-root card

A Tab5 using the littlefs/COW M5FS root also mounts `/assets`. This is a separate
bulk-file directory on the same littlefs volume; it does not consume the 8 MiB
logical M5FS root or expose `base.img` / delta banks. All littlefs public calls
share a kernel mutex, including root-store operations. There is no formatting or
root-volume resizing. FAT SD use remains available when no native root is active.

`quake receive BYTES CRC32hex` selects `/assets/quake/id1/pak0.pak` when the native
SD root is active, or the original `/sd/id1/pak0.pak` on FAT. Native destination
directories are created on demand. The bounded transfer refuses an existing
file, checks incoming CRC and removes its new destination on an ordinary error.
After an interrupted/power-lost transfer, inspect and remove any incomplete file
before retrying; upload is not a resumable or atomic publication protocol.
`quake verify` re-reads the stored file and prints its size and CRC. Plain `quake`
selects the matching storage base automatically; `/sd` and `/assets/quake` can
also be chosen explicitly. Gameplay remains read-only. This provides storage for
the base PAK; specific third-party mods and registered data are separate inputs
and have not been qualified. No game files are included in the repository.

For a repeatable transfer, quit any running app and use a Python environment
with pyserial (the ESP-IDF environment includes it):

```
python tools/quake/upload.py --port /dev/cu.usbmodem101 /path/to/pak0.pak
```

The helper refuses to bypass the firmware's no-overwrite check, reports progress,
and verifies stored size/CRC after transfer. On macOS it holds a temporary
`caffeinate` assertion for the transfer so host sleep cannot strand a binary
packet at the shell after the device's receive timeout. It closes the connection
and releases the assertion on exit; system power settings are unchanged.

## Frame profiling / exact damage updates

`quake bench` enters the Introduction (`start`) map directly for a reproducible
starting view. It does not download or write any game data. Normal `quake` still
starts with the usual demo/menu behavior. Serial Ctrl-T toggles the old extra
per-frame yield/sleep for diagnostics; physical keyboard routing is unchanged.
`quake perf` lines report wall time per engine frame, LCD refresh interrupts,
indexed copy/cache flush, input, explicit wait and remaining host work. These
are elapsed times including preemption, not isolated CPU samples. Filtered
(non-rendering) iterations still sleep; regular MeOS preemption is retained.

Quake's private 64,000-byte previous-index buffer tracks exact dirty source
bounds in each column. Unchanged HUD/background pixels need no framebuffer
writes. Palette changes force a complete repaint. Output remains 320x200 scaled
to the same 960x720 viewport with the same nearest-neighbor sample positions.
The host test compares whole physical framebuffers against the original blitter,
including moving scenes, sparse changes, palette edits and untouched rails.
The original full-copy API remains available to other clients.

Existing ignored `sdkconfig.quake` files are not overwritten by defaults. For
older DEBUG configs, use `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` and disable DEBUG,
as the integrated build defaults already specify. Explicit game/LCD hot-path
options retain speed optimization while the fixed app partition stays unchanged.

New Quake configurations select 128-byte L2 lines for framebuffer streaming.
The LCD writeback range now derives its alignment from the configured line size;
64-byte Doom/default builds retain their existing setting. Existing ignored Quake
configs also need `CONFIG_CACHE_L2_CACHE_LINE_128B=y` to reproduce this profile.
The L2 capacity stays 128 KiB; no SRAM is reassigned to a larger cache.

## Experimental PPA smooth backend

`quake smooth` enters the Introduction with an opt-in P4 PPA SRM backend. The
vertical sampling positions remain the same, while horizontal 3x expansion uses
the PPA's bilinear filter; this intentionally changes pixel-edge appearance.
The existing `quake` / `quake bench` exact nearest-neighbor backend is retained.
`quake ppatest` runs only an offscreen quadrant/guard test and exits, with no PAK
needed. A busy accelerator, failed test or hardware timeout retains CPU rendering.

This local kernel adapter uses LL register access rather than FreeRTOS PPA/DMA
queues. It only acquires clock-gated PPA and DMA2D modules, checks again atomically
before acquisition, masks peripheral interrupts and polls completion with a
bounded deadline. The test validates rotation, offsets and guard regions before
any game DMA writes the LCD. One conversion buffer and one test buffer (about
465 KiB) are kernel-owned reusable caches, allocated only on first smooth/test
use. They survive app exits. A hardware fault disables this backend until reboot
and quarantines those buffers, preventing late AXI operations from touching
recycled app allocations. App cleanup waits/resets its owned hardware before
restoring shell display/input. This is not an accelerator-sharing implementation.

The PPA stage is asynchronous: the next game computation can overlap DMA; source
reuse waits for the previous job. In `ppa-smooth` profile lines, `copy` includes
CPU staging, cache maintenance and any previous-job wait, not GPU-exclusive time.
Hardware LCD refresh and engine frame counts remain separate measurements.

Reference: ESP-IDF v5.4 PPA SRM and DMA2D LL/driver implementation; Espressif's
[PPA documentation](https://docs.espressif.com/projects/esp-idf/en/release-v5.4/esp32p4/api-reference/peripherals/ppa.html).

## Direct combat start and effect audio

`quake e1m1` starts the shareware Episode 1 combat map directly; `quake smooth e1m1` selects the same map with PPA rendering. The Introduction (`start`) used
by `quake bench` and plain `quake smooth` has no combat at its initial location.
Map names accept lowercase letters, digits and underscores. Original engine
registered-data checks remain intact; arbitrary third-party mods are unqualified.

The native backend mixes PAK WAV effects at 11025 Hz and converts stereo PCM to
mono for the Tab5 speaker at MeOS's 24000 Hz output rate. The audio worker only
copies integer PCM, without engine, filesystem, allocation or floating-point calls.
Consumed samples are cleared to avoid replaying stale sound during loading stalls.
Normal and forced app cleanup close audio before resetting the engine arena.
`quake smooth e1m1 --silent` disables effects for comparative performance runs.
Music/CD playback remains disabled; this backend implements effects only.

The shared Tab5 audio driver now derives descriptor and PCM alignment from the
configured L2 cache-line size (64 bytes in default/Doom builds, 128 in Quake).
Host ring tests cover rational conversion, wrap, chunk continuity and stall silence.
Physical E1M1 gameplay, nonzero mixer output, DMA progression with zero reported
underruns/errors and normal-exit cleanup passed on 2026-10-24. Audible effect output
was confirmed by the user. See `audio-device-2026-10-24.json`.

## Follow-up profiling (hardware measurement pending)

The next Quake image reports `quake phases` every 100 presented engine frames:
command execution, local server, client processing, rendering (including display
presentation) and game-task effect mixing, in microseconds per completed host
frame. These are elapsed times including preemption; the audio-worker callback
is not isolated CPU time. `quake io` reports read bytes and open/read/seek
call counts and elapsed microseconds, including failed operations. IO time is
nested within the host phases and must not be added to them. The first IO report
also includes Host_Init data reads; following reports reset interval totals.
`quake load` measures Host_Init through the first presented frame with complete
client signon, covering map loading for direct-map commands. This is a readiness
proxy, not a camera or input-latency measurement. No per-frame serial trace is
emitted. The hooks do not change frame scheduling or gameplay arithmetic.

Four float-to-integer rasterizer ceilings avoid software double/libm calls, with
exact integer results for finite, int-range inputs. Boundary/denormal cases, one
million reference cases and the same 130-frame ASan image pass. Native speed and
load-time comparisons await a coordinated device session after the parallel
SDMMC work; no firmware was flashed for this follow-up.

## Near-clamp and surface-cache follow-up (2026-10-25)

Native disassembly already lowers the candidate `1.0 / float` expressions to
`fdiv.s`; those expressions were left intact. Three projection near-clamp tests
now compare with `(float)NEAR_CLIP`. The assignment already stored that rounded
float, so the result is identical even at the adjacent float below the double
threshold (the original assignment there is a no-op). One million arbitrary
float bit patterns plus 128 adjacent boundary inputs match, including NaNs,
infinities and signed zero where encountered. Software double-conversion and
less-than helper call sites in r_draw fall from four each to one each; backface
and surface-ordering precision is unchanged. A temporary original-predicate
headless build matches the optimized 1,187-frame gameplay rolling hash.

`quake cache` reports interval hits, rebuilds, allocation requests, live-entry
evictions, arena wraps, thrash-flag transitions, rebuilt pixels and padded
allocation-request bytes. `quake cache reasons` counts existing entries rebuilt
for current/previous dynamic lighting, texture animation or lightstyle changes.
Reasons may overlap; entries absent after eviction are counted as allocations,
not attributed to their former reason. Cache capacity and replacement remain
unchanged. The counters are diagnostic instrumentation with some CPU overhead.

Host sweep (same demo, 1,200 host steps, sound disabled; includes warmup):

| Cache KiB | Hits | Rebuilds | Evictions | Rebuilt pixels |
| --- | ---: | ---: | ---: | ---: |
| 600 | 55,826 | 22,470 | 3,868 | 70,269,092 |
| 1024 | 56,614 | 21,682 | 2,678 | 68,236,248 |
| 2048 | 57,108 | 21,188 | 1,733 | 67,145,984 |

All three capacities produced the same gameplay rolling FNV-1a hash and frame-20
SHA256. The startup console is excluded from the rolling hash because its cache
size text intentionally differs. The 2 MiB variant reduces rebuilds by about
5.7% and rebuilt pixels by 4.4%; dynamic-light invalidation is common. This is not
a measured FPS gain. The native default remains 600 KiB pending device timing.
Raw results and native image identity: `cache-host-2026-10-25.json`.

To reproduce with separately supplied assets:

```
python3 tools/quake/headless.py /path/to/pak0.pak --steps 1200 --surface-cache-kib 600
python3 tools/quake/headless.py /path/to/pak0.pak --steps 1200 --surface-cache-kib 1024
python3 tools/quake/headless.py /path/to/pak0.pak --steps 1200 --surface-cache-kib 2048
python3 tools/quake/headless.py /path/to/pak0.pak --steps 1200 --reference-near-clamp
```

The original-predicate option modifies only a temporary source copy, not the
checkout. These are host correctness/counter tests, not physical speed evidence.

## Exact column-copy follow-up — 2026-10-25

Congruent halfword-offset source/destination columns now peel the first pixel
and keep the word-copy path; other alignments retain memcpy. Three identical
physical rows use one SRAM load per word, broadcasting to all three destinations.
The exact damage scan compares four aligned indices at once and updates byte
bounds only for differences; unaligned buffers keep the original byte scan.
Viewport, sampling, palette behavior and the 48 KiB task stack are unchanged.

Host tests compare all lengths 0..720 and 16 halfword alignment combinations
for single/triplet copies against memcpy, including untouched guards. The delta
blitter is compared against an independent generic RGB scaler for 100 frames,
including odd-start motion and unaligned inputs. Quake/Doom host checks and both
native builds pass, as does the clean-source simulator check.

Physical full-view `timedemo demo1;toggleconsole` (effects enabled, CPU rendering,
969 frames per run): baseline 24.6 / 24.6 FPS, peeled-copy + word-scan candidate
24.8 / 24.8, triplet broadcast 25.0 / 25.0. The first console-visible baseline
run was excluded. Reported gain is about 1.6%, not 30 FPS. Native memory bandwidth
and full-frame writes still require larger improvements; counters alone do not
establish a particular hardware bandwidth limit. Detailed observations and image
identity: `columns-device-2026-10-25.json`.

A late launch failed when the largest free internal block was 47 KiB, below the
unchanged 48 KiB stack request. Benchmark launches used the shell-ready startup
window on reset; the failure return code is now printed instead of silently
returning to the prompt. This adds diagnostics, not a fragmentation fix.

## Experimental exact DMA2D transfer

`quake dma e1m1` selects a DMA2D memory-to-memory backend. Normal `quake` still
uses CPU transfer; `smooth` retains the separate PPA bilinear prototype. These
backends cannot be selected together. Engine rendering, palette, 320x200 source,
960x720 viewport and nearest-neighbor sample positions remain unchanged.

The exact backend reuses CPU damage bounds and expands one column into each of
two alternating 1.5 KiB kernel-owned SRAM buffers. Three linked descriptor pairs
copy the same RGB565 column into the rotated framebuffer. The next column is
prepared while the previous transfer runs. Descriptor cache lines are separate;
viewport cache lines are flushed/invalidated before DMA, with no intervening CPU
writes inside the claimed viewport. Dirty ranges round outwards to word-aligned
boundaries, recomputing adjacent unchanged pixels identically. There is no large
RGB staging image or PPA filtering. Waiting for DMA completion is polling, so
scheduled time includes busy-waiting; this is not an asynchronous CPU-idle API.

Only a clock-gated DMA2D module is acquired, atomically against other users. A
bounded wait, descriptor error or failed offscreen test retains/returns to CPU
transfer; failure disables this experimental backend until reset. Kernel buffers
and descriptors survive app reclamation. Cleanup drains/resets its owned engine
before releasing the display. Active-burst forced kills and injected faults
remain physical qualification gaps, despite normal quit/relaunch checks passing.

Diagnostics (no PAK required):

- `quake dmatest`: offscreen linear/triplet copy with guard checks.
- `quake blitbench`: static/full/odd-band CPU transfer timings.
- `quake dmabench`: CPU and DMA transfer timings/checksums in one app lifetime.
- `quake dmaview`: holds readable color bars, grid, checkerboard and corner marks.
  Space switches CPU/DMA; A moves/pauses a bar; Ctrl-C or EXIT closes. `MEM PASS`
  means a full viewport pixel comparison passed, not that scanout was observed.
  The device screen must also be inspected for tearing or other visible errors.

Generate the static reference without game data or extra Python packages:

```
python3 tools/quake/visual_reference.py /tmp/dma-reference.png
```

Its expected physical-viewport FNV32 is `986b6f69`, matching CPU and DMA readback.
The initial performance benchmark's noisy pattern was ambiguous to the user;
static/moving diagnostic patterns and actual Quake rendering subsequently passed
user visual confirmation. A checksum alone is not visual acceptance.

On the identified Tab5 prototype, full-view `timedemo demo1;toggleconsole` with
effects and 969 frames gave 25.0 FPS twice for CPU and 32.0 FPS four times for DMA
(two later runs were for filming), about 28% improvement. This is demo-average
performance, not constant 30 FPS for every scene. Measurements identify the
prototype image and source base; the final PR also integrates later SD/Wi-Fi
changes and must not be described as an exact-head hardware measurement. See
`dma-device-2026-10-25.json` for evidence and remaining gates.

## Scheduled-time accounting

The scheduler now accumulates scheduled wall time per PID at actual context
switches. `task_current_runtime_us()` includes the current interval;
`task_runtime_snapshot()` copies live task counters under the IRQ lock. The old
`task_snapshot()`/`task_info_t` ABI is unchanged, as are priority/round-robin and
sleep policies. Counters include kernel and interrupt work charged to the task
at the switch boundary; they do not isolate instruction execution.

`quake cpu` reports interval time for the game and other stable live PIDs. New
PIDs need a baseline, and tasks disappearing between snapshots are omitted;
`other-live` is not a complete system CPU ledger. The first interval is omitted.
The measured replay showed about 1 ms/frame in other tasks, with keyboard polling
the largest listed contributor. Host scheduler tests cover ongoing/same-task
intervals, snapshots, reclaim and the 32-bit time boundary; stack tests verify
counter reset on TCB reuse. Foundation CI runs these tests.

## 512x300 baseline configuration

The default remains 320x200 -> 960x720. A separate build selects 512x300 ->
1024x600, centered at (128,60), with narrower control rails outside the viewport:

```
M5OS_QUAKE_RESOLUTION=512x300 sh tools/quake/build.sh
```

Outputs are `build-quake-512x300` and ignored `sdkconfig.quake-512x300`; the
existing low-resolution build/config are retained. The engine and adapter share
`m5os/quake_video.h`; compile definitions are checked in both native build trees.
The baseline keeps the same scalar renderer, 12 MiB engine arena, 48 KiB stack,
600 KiB surface cache, audio settings and exact DMA design. The cache is now
explicitly fixed at the old low-resolution value to avoid the engine silently
increasing it with pixel count. Source buffers grow with the selected resolution.

This is resolution adaptation, not a new renderer optimization. High-resolution
CPU transfer uses the same word damage scan and column-copy scheme; exact DMA
uses twofold expansion and two destination rows. Bounds use 16 bits because
source rows 256..299 cannot fit the old 8-bit bounds. Low-resolution expansion
and blitting remain their original branches. PPA `smooth` is rejected for this
configuration because its separate stage/filter path is still 320x200-only.

After a coordinated app-only flash, use `quake dma e1m1` (or `quake e1m1` for
CPU). `quake dmaview` verifies source pixels against the full displayed viewport;
its high-resolution static reference can be generated with:

```
python3 tools/quake/visual_reference.py /tmp/reference-512.png --resolution 512x300
python3 tools/quake/headless.py /path/to/pak0.pak --resolution 512x300 --steps 1200
```

Baseline device procedure: record the image hash, selected resolution/startup
message, clock/cache settings, load time and memory; run full-view
`timedemo demo1;toggleconsole` twice with effect audio for each CPU/DMA mode;
check display, input, audio, stack guards and normal quit/relaunch. Record 969-frame
means and interval lows separately. Do not change renderer math, cache size,
compiler options, transfer batching or scheduling while gathering this baseline.

Host checks pass at both resolutions, including unaligned buffers, palette
changes, rows above 255, untouched rails/margins and input routing. The high
engine completed 1,200 ASan host steps; the low 130-frame image remains exactly
`88b7f83db7da86e371452ed78ddefd1788701f5c356f6380cb30dd5296a6c06a`.
Both native builds, Doom host checks and the clean-source simulator pass.
`hires-host-2026-10-25.json` records these preparation results.

Physical Tab5 measurements on 2026-10-26 use source `f4e9ddb`, CPU 360 MHz,
128 KiB L2 cache, the existing 600 KiB surface cache and effect audio. Full-view
969-frame demo1 returned **10.7 / 10.7 FPS on CPU** and **5.7 / 5.7 FPS on DMA**.
The static start view (~23 FPS DMA) is not representative of the demo. The
high-resolution DMA path spends roughly 130 ms/frame in transfer during moving
intervals; it is slower than CPU here. The 150 KiB indexed image exceeds the
128 KiB cache, making column traversal/cache behavior a next investigation,
not a proven sole cause. No renderer/cache/compiler optimization was applied
while measuring. The earlier 320x200 DMA result was 32.0 FPS; its different
pixel count and viewport prevent a same-workload performance comparison.

Static CPU/DMA comparisons match the independent reference (`a4258a41`), and
moving DMA comparisons report zero mismatches. Stack guards remain intact;
normal exits report zero audio underruns/errors. Physical display, sound and
controls still require user acceptance for this resolution. Detailed image
identity and measurements are in `hires-device-2026-10-26.json`.

## High-resolution column staging — 2026-10-26

A 512x300 indexed frame occupies 150 KiB, exceeding the 128 KiB L2 cache.
Reading each full column separately wastes adjacent PSRAM pixels when cache
lines are evicted. The high-resolution CPU and DMA paths now read 64 adjacent
columns row by row into a 19,200-byte internal-stack tile, then expand columns
from that tile. DMA only receives the existing persistent kernel RGB buffers;
it never reads this stack tile. The 320x200 CPU/DMA branches are unchanged.

Full-view demo1, 969 frames per run, effect audio, two repeats each:

| 512x300 transfer | CPU FPS | DMA FPS | Tile bytes |
| --- | --- | --- | --- |
| Baseline | 10.7 / 10.7 | 5.7 / 5.7 | 0 |
| 16 columns | 14.1 / 14.1 | 16.0 / 16.0 | 4,800 |
| 64 columns (selected) | 15.9 / 15.9 | 17.5 / 17.5 | 19,200 |

The selected DMA candidate improves its own baseline by about 3.1x, and the
CPU candidate by about 49%. This does not reach 30 FPS. Representative moving
DMA intervals fall from roughly 130 ms transfer to 17–18 ms; renderer/other
work remains significant. No renderer math, clocks, cache capacity or compiler
options changed.

ASan/UBSan checks compare the full framebuffer to an independent RGB expansion,
including palette changes, sparse damage, unaligned input, rows above 255,
partial staging tiles and untouched margins. Native static/moving CPU/DMA
comparisons report zero mismatches; static FNV32 remains `a4258a41`. The user
confirmed normal display on the 16-column candidate and normal display/controls
on the final 64-column DMA game after relaunch. Sampled stack usage peaks
at 30,896 / 49,152 bytes with intact guards. Normal measured CPU/DMA exits
report zero audio underruns/errors. Repeated normal game exits/relaunches pass.

An existing late-start internal-memory fragmentation failure also occurred
after diagnostics. Starting the game early after reset succeeded; this change
does not fix allocator fragmentation or reduce the requested stack.
`hires-tile16-device-2026-10-26.json` and
`hires-tile64-device-2026-10-26.json` retain exact image identities and results.
CPU remains the default; use `quake dma e1m1` for the selected fast path.

## Two-column DMA chains — 2026-10-26

High-resolution exact DMA now chains two dirty source columns (four physical
rows) per hardware submission. Alternating kernel-owned source banks preserve
packing/transfer overlap, including sparse columns and final partial batches.
The next chain waits for completion before changing shared descriptors. Each
600-pixel expanded column is padded to 640 pixels for cache-line alignment.
Source payload is 5,120 bytes in a once-only 5,247-byte internal-heap allocation;
DMA never references an app allocation or task stack. Allocation failure
releases the claim and retains CPU rendering. Existing timeout/clock shutdown
semantics are unchanged. Low-resolution DMA still submits one source column.

With the same 360 MHz CPU, 128 KiB L2, 600 KiB surface cache and effect audio,
full-view 969-frame demo1 returns baseline **17.5 / 17.5 FPS** (55.3 s each)
and two-column DMA **18.4 / 18.4 FPS** (52.7 s each), about 5% faster.
Baseline source `7c343f6` includes the concurrent SD sector-cache improvement;
selected source is `333a366`. Low-resolution compatibility returns
**32.6 / 32.6 FPS** with effect audio. Native CPU/DMA static/moving comparisons
match their independent references, and selected normal exit reports zero
audio underruns/errors with nonzero PCM output. Stack guards pass at a sampled
peak of 29,696 / 49,152 bytes.

Four-column storage caused an audio-buffer allocation failure in a prototype;
its silent 19.3 FPS is excluded. An intervening two-column attempt yielded no
result row and is excluded; two other completed runs match. The known
post-diagnostic stack-fragmentation failure still requires an early launch
after reset in some cases. See `dma-batch-device-2026-10-26.json` for exact
identities and qualification limits. Human final display/audio acceptance is
separate from the native pixel comparison and nonzero PCM counters.

Host ASan/UBSan tests run the actual driver with delayed DMA completion. They
check descriptor/source bank lifetime, variable span lengths, sparse/partial
chains, rows above 255, full framebuffer/margin guards, palette repaint,
allocation failure fallback and once-only buffer reuse at both resolutions.

## Optional renderer scope profiling

Build a separate diagnostic image without changing the normal build/config:

```sh
M5OS_QUAKE_RESOLUTION=512x300 M5OS_QUAKE_DETAIL_PROFILE=1 sh tools/quake/build.sh
```

This produces `build-quake-512x300-profile` and ignored
`sdkconfig.quake-512x300-profile` (copied from the ordinary config on first use
when available). Normal builds omit the timer macros entirely; native archive
checks confirm no renderer references to `quake_profile_detail` in that build.
CI builds both normal resolutions plus the 512x300 diagnostic variant.

Every 100 presented frames, `quake detail outer` reports frame setup/visibility,
world edge processing, entities, view model, particles and water-warp times.
`quake detail nested` reports surface-cache lookup/rebuild, surface generation,
lightmap generation, perspective spans, depth spans, sky and water spans.

These are inclusive **wall** times, including preemption/interrupts. World edge
processing contains surface-cache and span work; cache time contains surface
generation, which contains lightmaps. Do not add overlapping timers together.
The timer-pair count is per report interval, and each time is divided by the
completed-frame count used by the existing phase report. The startup clock
calibration is an observed average including loop/IRQ overhead, not a guaranteed
latency bound. It does not account for all bookkeeping overhead.

Compare complete 969-frame `timedemo demo1;toggleconsole` runs with the console
closed and effect audio actually initialized. Record diagnostic FPS separately
from normal FPS to estimate measurement disturbance. Keep clocks, cache size,
renderer math, surface-cache budget and transfer mode fixed. Subtracting nested
timings can guide investigation, but these measurements are not a proof of
exclusive CPU time. Do not claim an optimization from host timing.

The host harness accepts `--profile-render`. Enabled/disabled 120-step runs
produce the same 101-frame gameplay hash `de60dcf25da70b2b` and frame-20 image
SHA-256 `1d628a0abd6ee31e11620efa3c91d6358a6b62026cbc7cfd75eaf82c521b7ac9`.
The diagnostic 1,200-step run also matches the previous 1,181-frame gameplay
hash `bfc101a42954ef9a`. Device timing is pending coordinated access: the first
flash attempt could not lock USB while a Wi-Fi/SD coexistence test was running;
it stopped before reading/writing the partition.

## Experimental depth-buffer pitch (host-qualified only)

The default remains zero padding. Optional high-resolution experiments add
32 or 64 **shorts** to each depth row, changing its byte pitch from 1,024 to
1,088 or 1,152. The visible indexed image stays packed at 512x300. This tests
a cache-layout hypothesis; no hardware speedup has been measured. Additional
PSRAM is 19,200 or 38,400 bytes through the existing private allocation path;
internal heap/audio allocation is unchanged.

```sh
M5OS_QUAKE_RESOLUTION=512x300 M5OS_QUAKE_ZPAD=32 sh tools/quake/build.sh
M5OS_QUAKE_RESOLUTION=512x300 M5OS_QUAKE_ZPAD=64 M5OS_QUAKE_DETAIL_PROFILE=1 sh tools/quake/build.sh
```

Outputs/configs gain `-zpad32` or `-zpad64` after any `-profile` suffix. Base
configs are copied on first use, so normal outputs/configs stay separate.
Unsupported values and nonzero padding in a native 320x200 build are rejected.
The engine and adapter share the selected pitch, and startup identifies the
experimental depth stride. CI retains existing normal/profile builds and adds
both candidate combinations.

Only allocation and the scalar depth row/table setup change. All compiled
depth consumers use `d_zwidth` or `zspantable`; the legacy recursive line path
inside `#if 0` is not supported. No rendering arithmetic or DMA layout changes.

Five ASan real-engine runs (1,200 steps each) compare high-resolution padding
0/32/64 and low-resolution 0/64. At 512x300 all three match the 1,181-frame
color hash `bfc101a42954ef9a`, logical-depth hash `45a556bc1aa2751f`, frame-20
image and surface-cache events. Every presentation validates
the depth pitch; padded variants also check initially zero then canary-filled
columns, for 1,210 guard invocations per run. Low-resolution logical
stride remains 320 and its color/depth hashes also match.

Native default `VID_Init` and `D_ViewChanged` unlinked machine-text bytes match
the previous unpadded build. Both candidate native builds and stack checks,
Quake/Doom host suites and the clean-source simulator pass. Exact identities
and scope limits are in `zpad-host-2026-10-26.json`.

No device access or flash was attempted for these candidates. Keep padding
disabled until controlled sound-enabled, console-closed demo1 A/B runs can
measure FPS and the diagnostic depth-span/world times, then verify display,
controls, audio and normal quit/relaunch on Tab5. Host timing cannot establish
a cache-layout benefit on ESP32-P4. The headless harness supports `--zpad` for
repeatable color/depth comparisons without redistributing game data.

## Quiet physical renderer profiling — 2026-10-26

`M5OS_AUDIO_QUIET=1` adds a separate `-quiet` build/config suffix after any
profile/pitch suffix. It selects ES8388 DAC attenuation 191 (-95.5 dB, one step
above the -96 dB endpoint), with the normal PCM mixer and DMA still running.
Both DAC registers are read back while the amp is off; any mismatch aborts
audio initialization before amp enable. Normal builds retain the original
-18 dB setup; normal `audio_open` unlinked machine-text bytes match the prior
build (1,368 bytes). The quiet option does not implement a runtime volume API.

```sh
M5OS_QUAKE_RESOLUTION=512x300 M5OS_AUDIO_QUIET=1 sh tools/quake/build.sh
M5OS_QUAKE_RESOLUTION=512x300 M5OS_AUDIO_QUIET=1 M5OS_QUAKE_DETAIL_PROFILE=1 sh tools/quake/build.sh
```

Tab5 sound-enabled full-view demo1, 969 frames, fresh game startup each repeat:

| Build | Run 1 | Run 2 |
| --- | --- | --- |
| Normal, quiet | 52.3 s / 18.5 FPS | 52.4 s / 18.5 FPS |
| Detailed timers, quiet | 52.5 s / 18.4 FPS | 52.6 s / 18.4 FPS |

The observed diagnostic-image duration difference is about 0.4%, including
possible code-layout/scheduling effects; it is not an isolated timer-cost
measurement. All four exits report zero audio underruns/errors and positive
nonzero PCM. Sampled stack peaks are 29,920 and 29,888 / 49,152 bytes, guards OK.

Medians over 18 steady 100-completed-frame diagnostic intervals:

| Scope | ms/frame | Nesting |
| --- | ---: | --- |
| Display transfer/staging/cache maintenance | 14.81 | Separate adapter scope |
| World edges | 28.72 | Includes cache and span scopes below |
| Surface cache | 3.75 | Includes surface build |
| Surface build | 3.37 | Includes lightmap |
| Lightmap | 0.21 | Nested in build |
| Perspective texture spans | 11.84 | Inside world edges |
| Depth spans | 6.00 | Inside world edges |
| Entities | 2.78 | Separate outer scope |
| View model | 1.99 | Separate outer scope |

Do not sum nested scopes or treat these interval medians as exclusive CPU
time/full-run means. The measurements prioritize texture/depth spans over
unmeasured cache-capacity changes. Warp/sky/water timers were zero in the
selected steady intervals; this does not qualify those paths.

Initial USB-only resets left DMA2D's clock enabled, so ownership guards
retained CPU rendering. Those CPU fallback runs are excluded. An esptool
watchdog reset followed by an early game start restored exact DMA without
bypassing ownership checks. Startup rejection reasons now distinguish request/
alignment rejection, existing module clock, and source/self-test allocation
failures. Do not accept a DMA benchmark without its offscreen PASS and
`dma-exact` interval reports; audio startup/readback and nonzero PCM are also
required.

A quiet zpad32 image was written and verified, but the USB device disappeared
after its reset before a monitor/game benchmark could start. Zpad32/64 timings
remain unmeasured; USB reconnection is required to continue. No faster pitch
is selected. See `quiet-profile-device-2026-10-26.json` for exact firmware
identities, raw intervals and incomplete physical qualification.
