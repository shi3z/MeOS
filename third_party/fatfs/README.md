FatFs R0.16, ChaN. Source: https://elm-chan.org/fsw/ff/arc/ff16.zip
Archive SHA-256: `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`.

`ff.c`, `ff.h`, `diskio.h` and `ffunicode.c` are upstream source with line endings normalized.
The permissive redistribution terms and copyright notices are retained in the source headers.
`ffconf.h` enables read-only FAT12/16/32, tiny sector buffering, CP437 short names,
and VFAT long names translated to UTF-8 (255 UTF-16 units, 768-byte name buffer).
The static long-name scratch buffer is protected by the same MeOS mutex as all
FatFs operations. exFAT, formatting and write APIs remain disabled.
