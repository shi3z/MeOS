doomgeneric, https://github.com/ozkl/doomgeneric
Pinned commit: e257a09527062bfb986b6e5e0780839b6cbd5b71

The engine C sources selected by the upstream Makefile, plus headers and GPL-2.0 license, are vendored
without changes. Desktop backends and `i_system.c` are omitted from the build; MeOS implements the
system, graphics/input, memory and file interfaces separately. MeOS replaces i_sound.c with its own DMX/MUS audio backend in apps/doom/sound.c.
Only the optional Tab5 Doom build links this engine (`tools/doom/build.sh`). Game WADs are not vendored.
