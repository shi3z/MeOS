# Simulator dependency notices

This inventory describes the application simulator's declared source closure.
It does not choose or grant a license for MeOS's original code. No root project
license was found in the current checkout or at the upstream repository's
GitHub license endpoint during this preparation. Project licensing remains a
maintainer decision, including authority over existing contributors' code.

| Component | Local use | License evidence |
|---|---|---|
| SDL2 | External development/runtime library; not vendored in this source candidate | Upstream SDL2 zlib license, copied in `licenses/SDL2-LICENSE.txt` |
| font8x8 | `drivers/lcd/font8x8.c/.h`, ASCII bitmap font | Local header credits Daniel Hepper; upstream README declares Public Domain, copied in `licenses/font8x8-README.txt` |
| /efont/ 16px font | `drivers/lcd/font_ja16.c/.h` | Existing header credits /efont/ 2000–2003 and BSD 3-Clause. Upstream /efont/ notices, including component-font notices, are preserved in `licenses/efont-upstream-notices.txt` |
| U8g2 font format/decoder provenance | Compressed Japanese font and `u8g2_*` helpers in `font_ja16.c` | Upstream library license is preserved in `licenses/u8g2-LICENSE.txt`; font licensing is described separately by upstream |
| Mini kana/kanji dictionary | `drivers/ime/ime_mini_dict.c`, generated from `tools/ime/mini.skk` | The tracked `.skk` source declares its MeOS mini dictionary Public Domain. Generator and source are included |
| System C library/compiler runtime | Provided by host toolchain | Not vendored; binary redistribution must inventory the actual linked runtime |

Copied upstream documents and their exact URLs/hashes are recorded in
`licenses/SOURCES.json`. Existing source-file notices are preserved unchanged.
The full compressed-font generation chain and exact originating BDF remain to
be established; the existing header's attribution is not independent provenance
verification. Do not treat this inventory as a completed license clearance.

Large downloaded SKK dictionaries, Doom/game data, FreeRTOS, ESP-IDF and unrelated
vendored interpreters are not in the simulator source manifest. Source export
does not bundle SDL, SDL2-compat, SDL3 or system libraries. A future binary release
must audit its actual dependencies and provide their notices as appropriate.

Upstream references:

- https://github.com/libsdl-org/SDL/tree/SDL2
- https://github.com/dhepper/font8x8
- https://github.com/olikraus/u8g2/wiki/fntgrpefont
- https://github.com/olikraus/u8g2/blob/master/LICENSE

## BIZ UD Gothic writing font

The writing editor uses BIZ UD Gothic Regular 1.051 by Morisawa, distributed
under SIL Open Font License 1.1. Original source:
https://github.com/googlefonts/morisawa-biz-ud-gothic
The pinned TTF is rasterized at build-preparation time into 24px, 16-level
coverage data (`drivers/lcd/bizud24_data.inc`); the simulator and firmware render
the same generated data. The unmodified upstream license and copyright are in
`tools/fonts/licenses/BIZ-UD-OFL.txt`. No second typeface is merged or used for
fallback. See `tools/fonts/README.md` for the source checksum and regeneration.

The prose editor also uses a generated BIZ UD Gothic atlas under SIL OFL 1.1.
The pinned TTF source, generator and license are documented in `tools/fonts/README.md`
and `tools/fonts/licenses/BIZ-UD-OFL.txt`; the source manifest includes that notice.
