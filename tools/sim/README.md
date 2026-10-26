# MeOS App Simulator

This prototype runs the **real Tab5 Home, Scheme IDE or writing editor** in a
resizable 1280x720 SDL window on the host. It links the same launcher, Scheme
engine, editor model, DQ widgets, ASCII/Japanese
font data, keyboard HID decoder, romaji converter, kana/kanji converter and built-in
SKK dictionary used by the Tab5 build. Only the display, timing, heap, console and
VFS adapters are replaced. It neither flashes nor opens the physical Tab5.

This is an application simulator: it does not boot MeOS, execute RISC-V firmware,
or test the MeOS scheduler, interrupts, DMA, FPU context, MIPI, USB, SDIO or device
drivers. Native host speed does not predict Tab5 performance. BASIC and Doom are
not connected to this SDL frontend. Home launches no child apps in the simulator;
it tests rendering and selection only. Scheme and the writing
editor can run sequentially within one process, sharing the production display
lease, IME, framebuffer and host VFS adapter.

## Run

Requires a C11 compiler and SDL2 development files (`sdl2-config`). On macOS:

```sh
brew install sdl2  # only if missing
sh tools/sim/run.sh
```

The script builds `build-host-sim/session.XXXXXX/meos-sim` and opens the IDE.
Every build has a unique output directory, including concurrent sessions. Set
`M5OS_SIM_BUILD_ROOT` to choose a different build root.
`sh tools/sim/build.sh` builds without launching and prints the executable path. With no arguments,
it creates a fresh temporary workspace and prints its path. Workspaces are kept
after exit so saved source can be inspected. To reuse an isolated workspace:

```sh
mkdir -p /tmp/my-meos-workspace
sh tools/sim/run.sh --workspace /tmp/my-meos-workspace
sh tools/sim/run.sh --workspace /tmp/my-meos-workspace --source /home/workspace.scm
```

All MeOS paths are relative to that directory: `/home/workspace.scm` is the host
file `<workspace>/home/workspace.scm`. These host files survive simulator exit,
unlike the physical Tab5's current RAM-backed `/home`. The application's save
message still describes the physical board. Paths are normalized; symlinks are
refused at each directory and final-file boundary. Use a distinct workspace per
session. There is no shared SD card or device state.

Use the keyboard in the SDL window:

- `C-x C-b`: evaluate the demo and display its function graph.
- F2: enter a Scheme expression; F1 or `C-x x`: M-x commands with Tab completion.
- `C-x C-s`: save; `C-x C-c` or `C-q`: quit; unsaved source asks for confirmation.
- `Ctrl-Space`: toggle **MeOS's own IME**. Type `nihongo`, Space to convert,
  Space again to show/advance candidates, `1` to select 日本語, Enter to commit.
- Existing arrows, F6/F7/F9/F10 and Ctrl-G go through MeOS's HID/IME logic.
- F12 saves the current simulated framebuffer to `capture-PID-NNNN.bmp` in the
  `--screenshots` directory. The no-argument launcher creates that directory
  inside its fresh workspace. While recording, F12 also records the capture point.

The prototype deliberately takes physical key events through the existing US HID
mapping instead of host `SDL_TEXTINPUT`, so use the host's Latin input source.
In `--home` mode a left click follows the same press/release selection model as
Tab5 touch. Host-native Japanese composition, clipboard paste, gamepads, sound
and camera are not implemented. Windows can be resized, with the simulated
display scaled at its original aspect ratio. Closing the window invokes the
IDE's quit command and asks before discarding unsaved source.

## Record and replay

Record keys, modifiers, Caps Lock and F12 capture points:

```sh
mkdir -p /tmp/meos-record /tmp/meos-images-a
sh tools/sim/run.sh --workspace /tmp/meos-record --screenshots /tmp/meos-images-a \
    --record /tmp/meos-session.keys
```

The recording path must be a new file. Quit through the IDE to finish recording.
Then reproduce it in another workspace:

```sh
mkdir -p /tmp/meos-replay /tmp/meos-images-b
sh tools/sim/run.sh --workspace /tmp/meos-replay --screenshots /tmp/meos-images-b \
    --replay /tmp/meos-session.keys
```

Add `--headless` to run without a window. Replay ignores physical key events;
closing the window aborts replay with an error. The application must quit after
all actions are consumed; early exit or a stall fails. `REPLAY PASS` means that
the action sequence completed, not that its output matched an expected baseline.

Use the same code, initial source/files and dictionary in both runs. Recordings
contain no copy of the initial filesystem. They are plain text with one action
per line (comments start with `#`):

```text
key 0 27 1 0
key 80 5 1 0
shot 80 graph
```

`key DELAY_MS HID_USAGE MODIFIER_BITS CAPS_LOCK` records HID usages, not Unicode
text, so romaji and candidate selection are replayed through MeOS's own IME.
`shot DELAY_MS NAME` captures a named BMP. Capture names use letters, numbers,
hyphens and underscores. Limits are 4,096 actions, 60 seconds between actions and
five minutes of total recorded delays. Unsupported/oversized input is rejected
before the IDE starts. Replay allows five additional seconds for application work;
these host delays do not model real hardware timing.

`tools/sim/scenarios/editor.keys` is a small reusable fixture for modifier keys,
save, F12 and quit. The runner is currently connected to Scheme; the recording
format and parser do not depend on Scheme and can be reused with future targets.

## Replay regression check

```sh
sh tools/sim/replay_test.sh
```

This runs ASan/UBSan parser tests (including 2,000 deterministic malformed cases),
records the Japanese/graph smoke with UBSan, and replays it in a new process and
workspace. It compares saved source and all four BMPs **byte for byte**. It also
runs the modifier/F12 fixture and checks the saved `; ABC` prefix and capture.
The tested Mac produced identical images for initial display, graph, Japanese
candidates and final saved state. This is reproducibility on the host renderer,
not a claim of visual equivalence to physical Tab5 scanout.

## Automated proof

```sh
sh tools/sim/smoke.sh
```

This creates a fresh workspace, builds with UBSan and runs SDL's dummy video
backend. Scripted SDL keyboard events go through the same HID decoder and IME as
interactive events. The script evaluates the demo, displays a graph, types and
converts `nihongo` into 日本語, saves the modified source, evaluates again and
quits. It checks the saved text, actual IME drawing/commit activity, graph drawing
and normal display/console cleanup. Four BMP screenshots and the saved `.scm`
remain in the printed results directory. Assertions on those counters establish
activity, not visual equivalence to physical scanout.

To watch the same sequence and capture images:

```sh
mkdir -p /tmp/meos-demo /tmp/meos-images
sh tools/sim/run.sh --smoke --workspace /tmp/meos-demo --screenshots /tmp/meos-images
```

The smoke sequence overwrites only `/home/workspace.scm` inside its supplied
workspace. `--headless` selects dummy video; `--screenshots DIR` requires an
existing directory. An internal ten-second deadline fails stalled smoke runs.

`M5OS_SIM_SANITIZE=1` enables UBSan for the simulator. Interpreter/editor ASan and
UBSan coverage remains available via `sh tools/scheme/host_test.sh`. On the
tested Mac, linking ASan with the installed SDL2-compat library aborted in the
library initializer before `main`; the simulator was therefore checked with
UBSan, and the application model separately with ASan/UBSan.

Verified on the development Mac: visible-window smoke and headless smoke, real
MeOS Japanese candidate rendering, source save and screenshot inspection. Linux
is intended by the POSIX/SDL adapter but has not been executed here. Physical
keyboard delivery and full manual editing remain separate checks from scripted
SDL event playback.

## Source-only verification and publication boundary

`sh tools/sim/check-clean.sh` exports the 64 explicitly declared source, header,
test and notice files, creates a temporary candidate Git repository, clones it,
and runs the host/replay checks there. No ESP-IDF, Tab5 hardware, unrelated dirty
files, or changes to the main checkout's index are required. The candidate includes
explicitly listed shared MeOS dependencies; it is not an authorship claim over them.

The Linux/macOS workflow is in `.github/workflows/simulator.yml`. Local Mac checks
have passed; adding the workflow is not evidence of a hosted CI pass. New source
files must be added to `tools/sim/source-files.txt` before export succeeds.

Normal app exit returns zero. Startup, memory, display or source-load failure
returns nonzero, as do invalid replay, timeout and incomplete replay. Build and
replay test scripts honor `CC` and `SDL2_CONFIG` for host toolchain selection.

See [publication preparation](RELEASING.md) and
[third-party notices](THIRD_PARTY_NOTICES.md). The project license and exact
compressed-font generation provenance are still unresolved. Source candidates are
marked for license review; neither a source snapshot nor these notices constitute
a published or license-cleared release.

### Prose writing mode

`sh tools/sim/run.sh --write --workspace DIR --source /home/note.txt` opens the
24px BIZ UD editor in right-to-left vertical prose mode (create DIR first).
Ctrl-T toggles the proportional horizontal view. Ctrl-Space uses MeOS Japanese
input, Ctrl-S saves, Ctrl-Q exits, Ctrl-Z undoes the last edit. Missing characters
are boxed. See `user/write/README.md`. Run `sh tools/sim/write_test.sh` for a real
vertical/horizontal HID, IME and save replay. The default simulator target remains
the Scheme workspace.

### Tab5 Home mode

`sh tools/sim/run.sh --home --workspace DIR` opens the two-column Home launcher.
Use arrows and Enter, click a tile, or press Q/Esc to return to Shell. The host
simulator stops after a selection instead of launching firmware-only apps.
`sh tools/sim/home_test.sh` captures the initial, Scheme-selected and
Camera-selected frames and verifies keyboard navigation and clean Shell exit.

`sh tools/sim/scheme_ui_test.sh` opens Scheme's 24px BIZ UD workspace, captures
the BIZ UD F2 minibuffer, evaluates `(sqrt 81)`, and verifies that the REPL result
changes the rendered frame. Scheme and Write share the same proportional inline
preedit and two-column candidate-popup renderer.

## Repeated application handoff

`sh tools/sim/handoff_test.sh` alternates Scheme and the writing editor three times
inside one process. Each Scheme launch evaluates a graph; both applications enter
and save Japanese through MeOS's real IME. Expected files are compared byte for
byte. After each app exits, the test checks the production display lease, drawing
target, console pause state, input queue, VFS handle count and stable live heap.
It also tests an invalid UTF-8 source in a later stage. The clean-source check
runs this test on both Linux and macOS CI.

For custom sequences, repeat `--stage APP VFS_SOURCE HOST_REPLAY`, up to 16 stages:

```sh
sh tools/sim/run.sh --headless --workspace /tmp/my-meos-workspace \
  --screenshots /tmp/meos-images \
  --stage scheme /home/workspace.scm /tmp/scheme.keys \
  --stage write /home/note.txt /tmp/write.keys
```

Stages are exclusive with `--write`, `--source`, `--smoke`, `--record` and `--replay`.
Each stage must consume its replay and quit. Heap equality is measured after the
first app has warmed the shared IME; it catches retained per-app allocations,
not libc/SDL allocations or MeOS task-stack/DMA/FPU behavior. This is sequential
handoff, not concurrent firmware execution.

Screenshot actions wait until an application is idle for input, rather than
capturing during Scheme's evaluation cancellation polls. CI also repeats the
record/replay check with `M5OS_SIM_EVAL_POLL_DELAY_MS=2` to exercise slow evaluation
and verify byte-identical frames. This simulator-only delay does not change the
firmware or its Scheme evaluator.
