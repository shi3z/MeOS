# Tab5 ES7210 microphone bring-up (hardware unqualified)

`mic test` explicitly captures for three seconds. `mic test N` accepts 1..10
seconds. It prints raw-slot peak, mean absolute amplitude, clipping count,
frames, DMA blocks and errors. It does not start at boot, save a recording,
transmit audio or run in the background. The ring is cleared before being freed.
The existing input permission is required when called by an application.

The first implementation is exclusive: I2S0 + AHB DMA RX2 are reserved, so
capture refuses while the output driver holds I2S0. The speaker is muted before
capture. The app's session lock serializes capture/cleanup; app termination
cleans up before app memory is reclaimed. Hardware and partially initialized
codec state are stopped on failures. Shutdown-write failures are reported as
unconfirmed rather than treated as successful codec power-down.

Protocol: 48kHz, four 16-bit Philips TDM slots, 12.288MHz MCLK, 3.072MHz BCLK,
12dB PGA gain. ES7210 is the slave at system-I2C address 0x40. GPIOs are MCLK30,
BCLK27, WS29 and DIN28. Eight 256-frame DMA blocks use internal SRAM; descriptor
slots occupy separate 64-byte cache lines. Completed descriptors and PCM data
are invalidated before reading, and ownership is flushed when returned to DMA.
Descriptor errors/empty rings, short blocks and stalled input abort capture;
they are not hidden by automatic restarts. Raw slot-to-microphone/reference
mapping has NOT been qualified, and four slots do not mean four physical mics.
No acoustic echo cancellation or duplex support is claimed.

Host validation:

- `sh tools/microphone/host_test.sh`: ASan/UBSan PCM slot statistics, int16 bounds,
  accumulation beyond 32 bits, invalid lengths, initialization fail-fast at each
  codec operation, and best-effort shutdown even after NACK.
- `sh tools/peripheral/host_test.sh`: atomic conflicts and reservation ownership.
- ESP-IDF 5.4.4 Tab5 firmware build.

These do not execute I2S/DMA hardware. This is a draft until device evidence
confirms: audible-input correlation and slot mapping; expected frame rate and
clock/frame format; silent-input/rail behavior and clipping; 1/3/10-second
captures; repeated capture -> playback -> capture; forced app termination;
DMA/cache stability alongside display/touch/Wi-Fi; busy rejection without
altering ongoing playback. Never infer working audio merely from DMA progress.

References (register sequence attribution: Copyright 2023 Espressif Systems
(Shanghai) CO LTD, Apache-2.0; see LICENSE.es7210):

- https://github.com/espressif/esp-adf/blob/1c7aa251b937742f1308c4f3b9e7587034bc5fb7/components/esp_codec_dev/device/es7210/es7210.c
- https://github.com/m5stack/M5Tab5-UserDemo/blob/b4e356bc491ca070d54004718dad789c07d5fc93/platforms/tab5/components/m5stack_tab5/m5stack_tab5.c
- ESP-IDF v5.4.4 `components/hal/i2s_hal.c` (TDM slot programming) and the
  ESP32-P4 `i2s_ll.h` / `ahb_dma_ll.h` implementations.
