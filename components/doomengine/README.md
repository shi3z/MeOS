# MeOS engine boundary

Upstream C files are kept unmodified. `compat.h` redirects the engine's libc calls
to private MeOS allocation, logging and read-only IWAD functions; it must not be
included by the rest of the kernel. `i_system_meos.c` supplies the engine's system
hooks. The application owns allocations, so appd can reclaim a killed game too.

The engine was designed for a process lifetime. To support repeated launches,
BSS/common are explicitly zeroed and initialized writable data is snapshotted
before the first run and restored after each exit. ESP32-P4's generated linker
script expands the dram0_data fragment in two regions, so SURROUND there would
redefine the snapshot bounds around an empty second expansion. `isolate_data.py`
renames engine writable initializers after archive creation; `data.ld` collects
them in one SRAM section inside the IDF's used SRAM span. It must be passed before
the main linker script (see root CMakeLists.txt). `-msmall-data-limit=0` also keeps
engine small BSS inside the resettable external BSS range. MeOS's PSRAM arena
starts after the linker-allocated external BSS.

Only the engine and port use -O2. The normal board firmware does not include this
component's sources, external state or local game data.

Qualification on 2026-10-23: ESP32-P4 Tab5 revision 1.3, ESP-IDF 5.4.4.
The full Doom 1.9 shareware IWAD rendered E1M1 on hardware at roughly 12–13 fps.
Two timed launches returned to the same heap baseline. A further keyboard/serial
Q exit reclaimed 10,907,312 bytes, then another launch rendered successfully.
The private libc tests pass under ASan/UBSan; the host engine renders 120 textured
frames. The normal Tab5 build and existing hardware/touch/SD host tests pass.
LCD appearance and single-touch gameplay were subsequently confirmed by the user.
Multi-touch gameplay and physical keyboard operation need user checks;
SD-backed IWAD loading and forced termination have not been hardware qualified.
The current GNU linker emits INSERT/non-contiguous-region and early region
reference warnings for data.ld; the final map and hardware re-entry were checked
with this toolchain (data at 0x4ff123e0..0x4ff20ca0, before the IDF low heap).

Multi-touch update: ST712x exposes all valid sparse slots (up to ten), and GT911
exposes up to five contacts with its existing ready/acknowledge handshake. The
single-point reader remains a wrapper returning the first valid contact. The
Doom adapter combines touch and text-keyboard holds before emitting transitions.
ASan/UBSan host tests cover sparse multi-touch, partial release, duplicate button
contacts, slides, failed reports, GT911 cached frames and mixed input sources.

Audio update: the build now replaces upstream i_sound.c with apps/doom/sound.c,
which provides the same public I_* API and uses the MeOS DMX/MUS mixer. It does
not enable FEATURE_SOUND's SDL includes. The adapter lives outside the engine's
resettable sections and explicitly resets its state before each launch. appd
closes/mutes audio before resetting engine globals and reclaiming sample memory.
`doom --silent` keeps the original silent mode. Hardware and mixer details and
qualification are recorded in tools/audio/README.md.

Screen/control update: the 320x200 engine image is now scaled to 960x720 at x160,
restoring a 4:3 display aspect and using the full panel height. The 160px rails
hold left movement/turning controls and right USE/FIRE controls (large FIRE),
with MENU/ENTER/QUIT at the top. Drawing and input share apps/doom/layout.h.
The resized Tab5 blit precomputes source row offsets outside the pixel loop.
Host tests exercise the real blit helper under ASan/UBSan, including the whole
image, untouched side rails, clipping and invalid sizes. Input tests cover all
button boundaries, game-area/gap rejection and simultaneous movement/fire.
Hardware qualification of the new layout: 10-second fresh-boot launch rendered
123 frames, sent 461 audio blocks with zero detected errors, started a shot
sound, cleaned up to the prior PSRAM baseline and relaunched with audio.
Doom now grants APP_PERM_SPAWN because initial audio worker creation checks the
caller's permissions even when the new worker is kernel-owned; this fixes the
previous dependency on running `audio test` before Doom. New side-control
appearance/ergonomics still require the user's hands-on check.

Performance update: the indexed presentation adapter avoids the RGB888 roundtrip,
uses small SRAM tiles and preserves the exact unchanged HUD. The Doom build uses
-O3/unrolled display loops and -O2 GPIO/I2C helpers; normal firmware keeps its
prior optimization flags. See tools/doom/PERFORMANCE.md for matched-image tests,
30–31fps hardware measurements and the remaining device qualification scope.
Legacy I2C timings remain four microseconds; only ST runtime reports take the
fast path. GPIO cycle-counted delays are used only by that path. Boot bus recovery
and expander-reset retry prevent a failed LCD init from silently invalidating FPS
measurements. Doom also participates in the shared full-screen display lease.
