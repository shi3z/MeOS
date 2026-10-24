FatFs R0.16, ChaN. Source: https://elm-chan.org/fsw/ff/arc/ff16.zip
Archive SHA-256: `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`.

`ff.c`, `ff.h`, `diskio.h` and `ffunicode.c` are upstream source with line endings normalized.
The permissive redistribution terms and copyright notices are retained in the source headers.
`ffconf.h` enables FAT12/16/32 with read-only/read-write mounts, tiny sector buffering, CP437 short names,
and VFAT long names translated to UTF-8 (255 UTF-16 units, 768-byte name buffer).
The static long-name scratch buffer is protected by the same MeOS mutex as all
FatFs operations. exFAT, formatting and write APIs remain disabled.

Read/write mounts now use FF_FS_LOCK=16, synchronize file data/metadata before
write success, and reject further disk I/O after a media error until explicit
unmount/remount. `sd mount` defaults to read/write; `sd mount ro` preserves the
read-only mode. Formatting, erase/TRIM and exFAT remain disabled.
FAT is not journaled: removal or power loss during a write may damage files or
the filesystem. These changes do not provide atomic replacement or recovery.

Timestamps currently retain FF_FS_NORTC=1 (fixed build configuration date);
UTC/RTC timestamp integration is outside this change.
