# Writing editor

Tab5 shell: `write [FILE.txt]` (default `note.txt` in the current directory).
Simulator: `sh tools/sim/run.sh --write --workspace DIR --source /home/note.txt`.
Create DIR first; host files are confined to this workspace. F12 captures a BMP
when `--screenshots DIR` is provided.

BIZ UD Gothic Regular at 24 native pixels, 36px line pitch, Latin/Japanese widths,
soft wrap without inserted newlines, warm light background, app-owned MeOS IME
composition/candidate panel. Ctrl-Space toggles the IME; Space converts, a second
Space opens candidates; Enter commits. Ctrl-S saves, Ctrl-Q exits (repeat to discard
unsaved edits), Ctrl-Z undoes the last edit. Arrows, Home/End, PgUp/PgDn navigate.
Buffer limit 16 KiB, one undo snapshot. UTF-8 files and MeOS VFS are used.

Only BIZ UD is embedded. Missing glyphs show a box. Combining marks and IVS are
not shaped as sequences yet. Soft wrapping is by glyph width without Japanese
kinsoku rules. Font asset adds about 2.57 MB of read-only data; a full offscreen
RGB565 frame (about 1.8 MB) is allocated in PSRAM to avoid clear/redraw flicker.
It is independent of the Scheme/code editor's existing grid UI.

`sh tools/sim/write_test.sh` replays the real MeOS IME through HID input, saves
Japanese text, verifies exact UTF-8 file contents, and captures wrapping/candidates.
