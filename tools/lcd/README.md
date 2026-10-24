# Tab5 resized bitmap drawing

The RGB565 and 8-bit indexed APIs use nearest-neighbour resizing and clip the
destination to the screen. Indexed input uses a 256-entry RGB565 palette.
The current off-screen target is honored; screen drawing flushes only the
physical framebuffer columns touched. Source buffers must contain the stated
source dimensions and must not overlap the destination framebuffer.

Dimensions are bounded to 4096 source / 8192 destination pixels per axis.
The generic implementation uses constant stack space, without the large
scratch arrays or fixed-viewport optimization used in experimental game code.
No frame-rate improvement is claimed. Display leases and timing counters are
outside this change.

Run `sh tools/lcd/host_test.sh` for ASan/UBSan pixel-reference tests covering
RGB/indexed equivalence, scaling, rotation, clipping, untouched side areas,
prefix-row rendering and extreme coordinates. Physical panel/cache regression
remains required. Public APIs are Tab5-only, matching the existing drawing API.
