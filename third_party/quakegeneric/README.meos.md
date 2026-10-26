Quakegeneric engine snapshot from https://github.com/espressif/esp32-quake at fe81f2840bc658bc4b009e77b1c6302572446e46.
Engine inherits GPL-2.0-or-later notices. No game assets included.
Local header change: configurable framebuffer dimensions, default 320x200.
The Espressif ESP-IDF/FreeRTOS front end is not imported. Upstream generic platform source files are retained for provenance but not selected for this build.
Local renderer change: edge/surface scratch arrays are static (single engine task), placed in external BSS instead of consuming a large internal task stack.
Large fixed work buffers in common.c (PAK directory), model/edge/span/sprite/world rendering, client precache/console, and datagram generation are static resettable external BSS. The port runs one engine task; these functions are not made reentrant. GCC stack-usage reports are checked during the component build (16 KiB per-function ceiling; call chains/recursion still require runtime guards).

Performance follow-up: `D_DrawZSpans` keeps its already-FP32 intermediate in FP32
while scaling by exact powers of two. `D_PolysetSetUpForLineScan` uses signed
integer floor division for integer endpoints, repairing negative remainders.
Both avoid software double operations on the P4 without fast-math. A million
reference comparisons pass, and the 130-frame host render remains byte-identical.
The engine uses scalar `-O3 -fno-tree-vectorize -fno-strict-aliasing`; stack limits
remain enforced. These changes do not assign a license to the surrounding MeOS.
Vector length/normalization also uses an FP32 square root for its FP32 input and
output. GCC/Clang builds request `__builtin_sqrtf`, with the original double call
as the portable fallback. ESP32-P4 disassembly confirms `fsqrt.s`. A million
finite input comparisons and the same 130-frame image comparison remain exact.

Four rasterizer edge/light-gradient ceilings now use `Q_CeilFloatToInt` for
finite float inputs whose result is representable as int. Truncation plus a
positive-fraction correction exactly preserves integer ceil without double/libm
calls. Boundary/denormal tests and a million comparisons pass; the 130-frame
render remains byte-identical. MeOS-only host phase hooks aggregate command,
server, client, rendering and effect-mixer elapsed times; builds without the
forced compatibility header retain no-op hooks. No game arithmetic is changed
by profiling. Hardware speed impact of this follow-up remains unmeasured.

Three r_draw projection near-clamps compare at their float storage precision,
removing double comparison helpers without changing the resulting float values.
Backface/ordering tests retain their existing precision. Surface-cache hooks
count reuse, regeneration, allocation/eviction and invalidation reasons in the
MeOS profiler; other builds compile these hooks away. Cache capacity and
replacement behavior are unchanged. See tools/quake/cache-host-2026-10-25.json
for the host-only size sweep and original-clamp image comparison.

An opt-in 512x300 build uses the same renderer through shared compile-time video
configuration; the default stays 320x200. No additional renderer math changes or
parallelism are introduced for that baseline. Host smoke runs support both
resolutions; physical 512x300 performance remains pending.

Optional `MEOS_QUAKE_DETAIL_PROFILE` scopes around renderer calls report
inclusive wall-time breakdowns for world, entities, cache/lightmap generation
and span drawing. Normal builds compile these macros out; they do not change
renderer math, cache replacement or game state. See `tools/quake/README.md`.

An optional `M5OS_QUAKE_ZPAD` experiment (default 0; high-resolution 32/64)
changes only depth-buffer allocation and scalar depth row/table pitch. Visible
color packing and arithmetic are unchanged. Host color/depth equivalence and
padding canaries pass; native performance qualification remains deferred.
See `tools/quake/README.md` and `zpad-host-2026-10-26.json`.
