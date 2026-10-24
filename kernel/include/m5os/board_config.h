#pragma once
/*
 * What the board is, as the kernel needs to know it.  Exactly one of M5OS_BOARD_CORES3 /
 * M5OS_BOARD_CARDPUTER is defined by the build (tools/build.sh).
 */
#if defined(M5OS_BOARD_CARDPUTER)
#  define M5OS_BOARD_NAME     "M5Cardputer"
#  define M5OS_CPU_NAME       "ESP32-S3 (Xtensa LX7)"
#  define M5OS_DISPLAY_W      240          /* ST7789 135x240 panel used in landscape */
#  define M5OS_DISPLAY_H      135
#  define M5OS_HAS_TOUCH      0
#  define M5OS_HAS_KEYBOARD   1
#  define M5OS_HAS_SD         1
#elif defined(M5OS_BOARD_TAB5)
#  define M5OS_BOARD_NAME     "M5Stack Tab5"
#  define M5OS_CPU_NAME       "ESP32-P4 (RISC-V)"
#  define M5OS_DISPLAY_W      1280         /* 5" MIPI-DSI panel, used in landscape */
#  define M5OS_DISPLAY_H      720
#  define M5OS_HAS_TOUCH      0            /* window-manager touch path disabled; console touch uses tab5touch/osk separately */
#  define M5OS_HAS_KEYBOARD   1            /* a USB keyboard on the USB-A host port */
#  define M5OS_HAS_SD         0            /* no SD-backed M5FS root; explicit SPI/FAT mount at /sd */
#  define M5OS_PSRAM_RESERVE  (4u * 1024u * 1024u)   /* top of PSRAM left to the IDF heap: the DSI driver allocates its framebuffer there */
#  define M5OS_FBCON_SCALE    2            /* 1280x720 with 8x8 glyphs would be 160 columns of tiny text */
#  define M5OS_ARCH_RISCV     1
#elif defined(M5OS_BOARD_CORES3)
#  define M5OS_BOARD_NAME     "M5Stack CoreS3"
#  define M5OS_CPU_NAME       "ESP32-S3 (Xtensa LX7)"
#  define M5OS_DISPLAY_W      320
#  define M5OS_DISPLAY_H      240
#  define M5OS_HAS_TOUCH      1
#  define M5OS_HAS_KEYBOARD   0
#  define M5OS_HAS_SD         1
#else
#  error "no board selected: build with tools/build.sh [cores3|cardputer|tab5]"
#endif

#ifndef M5OS_FBCON_SCALE
#  define M5OS_FBCON_SCALE    1            /* console text size: glyph pixels per font pixel */
#endif
