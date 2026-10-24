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
