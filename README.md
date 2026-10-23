# MeOS

An educational and experimental lightweight operating system built from scratch for M5Stack devices (**M5Stack CoreS3**, **M5Cardputer**, and **M5Stack Tab5**).
MeOS implements its own **preemptive/cooperative scheduler, task management, timer, IPC message queues, memory allocator, VFS, custom filesystem (M5FS), dynamic binary loader (M5EX), GUI/TUI window system, camera driver, SDIO Wi-Fi stack, and SSH-2 client**—without ever starting FreeRTOS.

---

## Progress

| Phase | Feature | Status |
|---|---|---|
| 1 | Bare-metal boot, UART (USB-Serial-JTAG) output | ✅ Verified on hardware |
| 2 | Cooperative scheduler (`task_create/yield/sleep/exit`) | ✅ Verified on hardware (`ABAB…`, 100 ms sleep granularity) |
| 3 | Preemptive scheduler (CCOMPARE1 / CLIC tick ISR, custom vector table) | ✅ Verified on hardware (two CPU hogs share ~1:1; prio-3 task wakes within 210 ms under load) |
| 1.5 | LCD driver (`display_*` API) | ✅ Verified on hardware |
| 4 | Memory: boundary-tag free-list heap, separate internal SRAM / PSRAM arenas, `kmalloc*` | ✅ Verified on hardware (`selftest ALL PASS`) |
| 5 | Device abstraction (`device_open/read/write/ioctl`), blocking mutex, `uart`/`display`/`buttons` | ✅ Verified on hardware |
| 6 | M5FS custom filesystem + SD block device | ✅ M5FS verified on hardware over RAM disk |
| 7 | VFS (`/dev` = devfs, `/` = M5FS), mount table | ✅ Verified on hardware (`VFSTEST ALL PASS`) |
| 8 | Interactive Shell (`help history ps kill ls cat echo mem uptime run` + cursor-key history & line editing) | ✅ Verified on hardware |
| 9 | App lifecycle (`app_start/stop/list`, permissions, automatic resource reclamation) | ✅ Verified on hardware (`APPTEST ALL PASS`) |
| 10 | App loader (`M5EX` relocatable binary format, dynamic loading over serial/FS) | ✅ Verified on hardware (`M5EXTEST ALL PASS` + host push & run) |
| 11 | IPC (fixed-size per-task mailboxes, blocking/timeout send & receive) | ✅ Verified on hardware (`MSGTEST ALL PASS`) |
| 12 | Touch GUI (window manager, launcher, Terminal / Clock / Calculator / Snake) | ✅ Verified on hardware (`GUITEST ALL PASS`) |
| 13 | TUI Window System (`dqwin`), Full-Screen Filer (`files`), and Text Editor (`edit`) | ✅ Verified on M5Cardputer & M5Stack Tab5 |
| 14 | Camera App (`camera` / `snap`, MIPI-CSI SC202CS + ISP) & Filer BMP Image Viewer | ✅ Verified on M5Stack Tab5 |
| 15 | Graphical Wi-Fi Manager (`wifi`) & FreeRTOS-free ESP32-C6 SDIO / ESP-Hosted Stack | ✅ Verified on M5Stack Tab5 |
| 16 | SSH-2 Client (`ssh`, `curve25519-sha256` / `aes128-ctr` / `hmac-sha2-256`) + TCP/DNS Stack | ✅ Verified on M5Stack Tab5 |
| 17 | Japanese input method / FEP (romaji → kana → kanji, SKK dictionary, candidate list, phrase editing) | 🧪 Host-tested; not yet run on hardware |

---

## Supported Boards

| Feature | M5Stack CoreS3 | M5Cardputer (Original / ADV) | M5Stack Tab5 |
|---|---|---|---|
| **SoC** | ESP32-S3 (Xtensa LX7) + 8 MB PSRAM, 16 MB Flash | ESP32-S3FN8: 8 MB Flash, **no PSRAM** | **ESP32-P4 (Dual-core RISC-V @ 400 MHz)** + 32 MB PSRAM, 16 MB Flash + **ESP32-C6** co-processor |
| **Display** | ILI9342C 320×240 (SPI, via AXP2101/AW9523) | ST7789 135×240 in landscape (240×135) | ST7121 / ILI9881C 720×1280 MIPI-DSI in landscape (**1280×720**) |
| **Input** | Capacitive touch (FT6336U) | **Built-in Keyboard** (74HC138 matrix / TCA8418 I2C) | **Tab5 Keyboard** (I2C HID `0x6D`) or USB-A HID boot keyboard |
| **UI Mode** | Touch GUI (Window Manager + Launcher) | Full-screen console (`fbcon`) + TUI apps (`dqwin`) | High-res console (`fbcon` 80×45) + TUI apps (`files`, `edit`, `camera`, `wifi`, `ssh`) |
| **Camera** | — | — | **SC202CS** 1600×1200 MIPI-CSI + Hardware ISP + Auto-Gain/White-Balance |
| **Wi-Fi / Net** | — | — | **ESP32-C6** over SDIO Slot 1 (DHCPv4/ARP/ICMP/DNS/TCP + **SSH-2 Client**) |
| **Build Command** | `tools/build.sh cores3` | `tools/build.sh cardputer` | `tools/build.sh tab5` |

```sh
# Build and flash for M5Stack Tab5 (ESP32-P4)
tools/build.sh tab5 build
tools/build.sh tab5 flash -p /dev/cu.usbmodemXXXX

# Build and flash for M5Cardputer
tools/build.sh cardputer build
tools/build.sh cardputer flash -p /dev/cu.usbmodemXXXX

# Build and flash for M5Stack CoreS3
tools/build.sh cores3 build
tools/build.sh cores3 flash -p /dev/cu.usbmodemXXXX
```

Board-specific code is cleanly separated into `*_cores3.c`, `*_cardputer.c`, and `*_tab5.c` and selected via CMake (`M5OS_BOARD`).
Board capabilities (name, resolution, touch/keyboard availability) are exposed to the kernel through `kernel/include/m5os/board_config.h`.

---

## Build & Run

```sh
. tools/env.sh                                                   # Activate ESP-IDF virtualenv
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
python tools/monitor.py /dev/cu.usbmodemXXXX 5                   # View boot logs for 5 seconds
python tools/shell_test.py /dev/cu.usbmodemXXXX 'ps' 'ls /dev'   # Send commands to the shell and print output
screen /dev/cu.usbmodemXXXX 115200                               # Interactive serial session (exit: Ctrl-A K)
M5OS_SELFTEST=1 idf.py build                                     # Run full regression test suite at boot
```

---

## Architecture

```
 Power On
   │
   ▼
 ROM ──► Espressif 2nd-stage Bootloader ──► call_start_cpu0()        (IDF: cache / flash / clock / PSRAM)
                                              │
                                              ▼
                                       start_cpu0_default()          (IDF: peripheral & memory init)
                                              │ esp_startup_start_app()
                                              │   ▲ Normally calls vTaskStartScheduler()
                                              ▼   │ Intercepted via linker flag -Wl,--wrap
                                     __wrap_esp_startup_start_app()  kernel/boot/boot_hook.c
                                              │
                                              ▼
 ┌──────────────────────────────────── MeOS ────────────────────────────────────┐
 │ user/    shell  launcher  filer  edit  camera  wifi   apps/  hello clock ... │
 │ ──────────────────────────── Syscall-level APIs ──────────────────────────── │
 │ kernel/  task ─ scheduler ─ timer ─ memory ─ ipc ─ irq                       │
 │ fs/      vfs ── m5fs          drivers/  uart lcd kbd cam wifi sd ...         │
 │ ────────────────────── arch.h / hal_*.h (Porting Boundary) ───────────────── │
 │ hal/esp32s3/ (Xtensa LX7)     hal/esp32p4/ (RISC-V 32-bit CLIC)              │
 └──────────────────────────────────────────────────────────────────────────────┘
```

`kernel/` only depends on the abstract declarations in `kernel/include/m5os/arch.h` and `hal_*.h`. Porting MeOS to another SoC architecture only requires implementing `hal/<chip>/`.

---

## Rules for Running Without FreeRTOS

* `libfreertos.a` is linked because ESP-IDF startup components reference its symbols, but **the FreeRTOS scheduler is never started** (`vTaskStartScheduler` is stripped by `--gc-sections`).
* **Allowed from ESP-IDF**: ROM/bootloader/startup, linker scripts, hardware register headers (`soc/*_reg.h`, `soc/*_struct.h`), low-level inline HAL headers (`hal/*_ll.h`), cache sync (`esp_cache_msync`), and `esp_rom_*`.
* **Not used from ESP-IDF**: FreeRTOS tasks/queues/semaphores, `esp_timer`, `esp_ipc`, newlib `printf`/locks (which internally take FreeRTOS mutexes), or ESP-IDF VFS. All kernel/user formatting goes through our own lock-safe `kprintf` / `ksnprintf`.

---

## Kernel Subsystems

### 1. Cooperative & Preemptive Scheduler (Phases 2 & 3)
* **Xtensa LX7 (`hal/esp32s3/`)**:
  * Uses `window_spill4` to spill register windows to the stack and stores only `sp` in the TCB.
  * Points `VECBASE` to a custom 1 KiB vector table (`hal/esp32s3/vectors.S`) that handles Level-3 timer interrupts (`CCOMPARE1`, 10 ms tick) directly without touching FreeRTOS's `_frxt_int_enter/exit`.
  * Preserves the 16-byte Xtensa ABI base save area at the top of the interrupt frame and spills windows *before* decrementing `sp`.
* **RISC-V (`hal/esp32p4/`)**:
  * Uses the CLIC interrupt controller with a static executable vector table (`hal/esp32p4/vectors_p4.S`).
  * Ensures `mcause` bit 31 (`interrupt`) is set when restoring via `mret` so `mintstatus.mil` is properly restored across context switches.
* **Per-priority Round-Robin**: Maintains an independent round-robin cursor per priority level so frequent high-priority tasks (such as `touchd` or `kbdd`) never starve lower-slot tasks at the same priority.

### 2. Memory Management (Phase 4)
* **Dual Heap Arenas (`kernel/memory/heap.c`)**:
  * **Internal SRAM**: 192 KB static `.bss` arena (`kmalloc_internal` / `kmalloc_dma`) used for task stacks, DMA descriptors, and ISR-accessible structures.
  * **PSRAM**: Up to 8 MB (CoreS3) or 28 MB (Tab5) external SPIRAM arena (`kmalloc_psram`) for large buffers, RAM disks, camera frames, and images.
* **Allocator**: Boundary-tag free-list with 16-byte aligned headers (`magic`, `size`, `prev_size`), immediate left/right coalescing on `kfree()`, and double-free / corruption detection.

### 3. Device Layer & Mutexes (Phase 5)
* Applications access hardware through `device_open()`, `device_read()`, `device_write()`, and `device_ioctl()`—serialized per device via blocking kernel `mutex_t` (`kernel/ipc/mutex.c`).
* When a task is killed via `kill PID` or `app_stop()`, any mutexes held by that task (`task_t.held`) are automatically released so system services (like `kprintf`) never deadlock.

### 4. M5FS Filesystem & VFS (Phases 6 & 7)
* **M5FS (`fs/m5fs/m5fs.c`)**: Custom little-endian block filesystem (512-byte blocks):
  * Block 0: Superblock (`M5FS` magic, version, geometry, free block count, CRC32).
  * Bitmap blocks + 16 directory blocks (128 entries × 64 bytes) + data blocks.
  * Each file stores up to **4 extents** (`{start, count}`) of contiguous blocks, avoiding FAT chains.
* **VFS (`fs/vfs/vfs.c`)**: Longest-prefix mount table mounting `devfs` at `/dev` and `M5FS` at `/` (backed by SD card if a valid M5FS volume is present, or an automatic PSRAM/SRAM RAM disk with `/system`, `/apps`, and `/home`).

### 5. App Lifecycle, Permissions & M5EX Dynamic Loader (Phases 9 & 10)
* **Lifecycle (`kernel/app/app.c`)**: Separates user apps from kernel tasks. Tracks per-app permissions (`C`=console, `D`=display, `I`=input, `r`/`w`=file read/write, `s`=task spawn, `m`=IPC), open VFS handles, child tasks, and `umalloc()` memory blocks. When an app exits or is killed, the `appd` kernel daemon reclaims all owned tasks, files, windows, and heap allocations.
* **M5EX Loader (`kernel/loader/m5ex.c`)**: Relocatable binary format (`header(64B) | image | relocation_table`) with CRC32 validation and a capability-checked kernel function table (`m5_api_t`). New apps can be compiled on the host (`tools/m5exbuild.py`) and pushed live over USB serial (`tools/m5ex_push.py`).

### 6. IPC Message Queues & Touch GUI (Phases 11 & 12)
* **IPC (`kernel/ipc/msg.c`)**: Per-task 8-slot FIFO mailboxes of 32-byte messages (`sender`, `type`, `data[24]`) with kernel-stamped sender PIDs and blocking/timeout send & receive.
* **GUI (`kernel/gui/gui.c`)**: Full-screen stacked window manager for touch devices (CoreS3) with IPC event delivery (`DOWN`/`MOVE`/`UP`/`EXPOSE`/`CLOSE`), system bar (`Home` / `Next` / `Close`), Launcher, Terminal, Clock, Calculator, and Snake.

---

## M5Stack Tab5 Port (ESP32-P4, RISC-V)

Build: `tools/build.sh tab5 build` / Flash: `tools/build.sh tab5 flash -p <port>` (uses ESP-IDF 5.4.4).

Key hardware subsystems supported on M5Stack Tab5:
* **Display (`drivers/lcd/display_tab5.c`)**: 720×1280 MIPI-DSI panel (ST7121 / ILI9881C) driven in **1280×720 landscape** (180° rotated in software `put()` with `esp_cache_msync` writeback).
* **I/O Expanders (`hal/esp32p4/board_tab5.c`)**: Dual PI4IOE5V6408 I2C expanders (`0x43` / `0x44`) controlling LCD/touch reset, speaker enable, camera reset, USB 5V host power, and ESP32-C6 Wi-Fi power.
* **Keyboard (`drivers/tab5kbd`, `drivers/usbkbd`)**: Polls the official M5Stack Tab5 I2C Keyboard (`0x6D` on Ext.Port1 G0/G1) in HID mode, with automatic fallback to USB-A HID boot keyboards over the DWC2 host controller.

---

## TUI Window System (`dqwin`), Filer (`files`), and Text Editor (`edit`)

Keyboard-equipped boards (**M5Cardputer** and **M5Stack Tab5**) feature a Dragon Quest-inspired retro TUI window toolkit (`user/tui/dqwin.c`) with thick white rounded borders on a black background and a `▶` selection cursor. Layout uses character cells (`8×8` glyph × `M5OS_FBCON_SCALE`), scaling cleanly from 40 columns on Cardputer to 80×22 on Tab5, and works simultaneously on both the built-in LCD+keyboard and serial terminals.

### Full-Screen Filer (`files [DIR]`)
Implemented in `user/filer/filer.c` (inspired by VZ Editor's filer). Run `files` from the shell or launch `run files`.

| Key | Action |
|---|---|
| `↑` `↓` `←` `→` `PgUp` `PgDn` `Home` `End` | Navigate directory entries (`←`/`→` moves across columns) |
| `Enter` / `BS` | Enter directory, view `.bmp` image, or view text file / Go to parent directory |
| `Space` | Toggle file selection mark (`*`) |
| `C` / `M` | Copy / Move marked files (or file under cursor) |
| `D` | Delete file or empty directory (with confirmation dialog) |
| `K` | Create directory (`mkdir`) |
| `N` | Rename file |
| `V` | Text/Hex viewer (first 8 KB) |
| `P` | **Image Viewer** (renders 24-bit `.bmp` photos full-screen) |
| `A` | Launch **Camera App** directly from Filer and refresh directory on exit |
| `W` | Launch **Graphical Wi-Fi Settings App** directly from Filer |
| `S` | Launch **SSH-2 Client** interactive connection dialog directly from Filer |
| `E` / `F` | Open file in Text Editor / Create new file in Text Editor |
| `Q` / `ESC` | Exit Filer |

### Text Editor (`edit FILE`)
Implemented in `user/edit/edit.c`. Edits ASCII text files up to 32 KB with `Ctrl-S` to save, `Ctrl-Q` / `ESC` to quit (with unsaved changes prompt), arrow keys, `Home`/`End`, `PgUp`/`PgDn`, and 2-space Tab insertion.

---

## Camera App (`camera` / `snap`) & BMP Image Viewer

Implemented in `drivers/camera/camera_tab5.c` and `user/camera/camera_app.c` for M5Stack Tab5:
* **Hardware Pipeline**: Drives the onboard **SmartSens SC202CS** (1600×1200 RAW8 BGGR sensor at I2C `0x36`) over **MIPI-CSI** + **Hardware ISP** (configured with `ISP_COLOR_RAW8` → `ISP_COLOR_RGB565`) + **DW-GDMA** into PSRAM at 800×600 resolution, using a 24 MHz external clock routed out of `GPIO36` via LEDC.
* **Image Quality & Color Calibration**:
  * Configures SC202CS manual exposure + analog/digital coarse/fine gain and applies per-channel RGB white-balance gains during downscaling.
  * Properly handles ESP32-P4 ISP RGB565 byte order in both live preview and 24-bit `.bmp` encoding so reds, greens, and blues are natural and accurate.
* **Interactive Camera App (`camera` or `run camera`)**:
  * Live 640×480 preview centered on the 1280×720 display.
  * Press `Space` or `Enter` to capture and save a 320×240 24-bit uncompressed `.bmp` photo (`/home/CAP_0001.BMP`, `CAP_0002.BMP`, ...).
  * Press `V` to immediately open the last captured photo in the BMP viewer, or `Q`/`ESC` to return.
* **Headless CLI Snapshot (`snap [FILE]`)**: Captures a photo directly from the shell command line (after 5 warmup frames) and saves it to `/home/CAP_XXXX.BMP` or the specified path.
* **Filer BMP Viewer (`user/filer/bmpview.c`)**: Selecting any `.bmp` file in `files` and pressing `Enter` or `P` renders the image scaled up to 3× (960×720) with metadata overlay.

---

## Graphical Wi-Fi Manager (`wifi`) & ESP32-C6 SDIO Stack

Implemented in `drivers/wifi/wifi_tab5.c` and `user/wifi/wifi_app.c` for M5Stack Tab5:

### FreeRTOS-Free ESP32-C6 SDIO & ESP-Hosted Driver (`drivers/wifi/wifi_tab5.c`)
* **Polled SDMMC Slot 1 Driver**: Drives ESP32-P4's SDMMC Host Slot 1 (`CLK=GPIO12`, `CMD=GPIO13`, `D0..D3=GPIO11..8`, `C6_RST=GPIO15`) in 4-bit 20 MHz SDIO mode using polled IDMAC descriptors and explicit L1/L2 cache synchronization (`esp_cache_msync`)—with zero FreeRTOS dependencies.
* **ESP-Hosted-MCU Protocol**: Implements the 12-byte `esp_payload_header` streaming transport, slave token flow control, `ESP_PRIV_IF` capability handshake, and a lightweight Protobuf wire-format encoder/decoder over `ESP_SERIAL_IF` (`RPCRsp` / `RPCEvt`).
* **Built-in DHCPv4, ARP, ICMP, UDP DNS & TCP Client Stack (`ESP_STA_IF`)**: Handles Ethernet II frames directly over SDIO Function 1, automatically acquiring an IPv4 address, netmask,gateway, and DNS server via DHCP (`Discover` → `Offer` → `Request` → `Ack`), resolving hostnames via UDP DNS (`wifi_resolve_host`), and providing a reliable TCP client state machine (`wifi_tcp_connect/send/recv/close`) with a 16 KB receive ring buffer and retransmission.
* **Persistent Credentials & Auto-Connect (`wifid`)**:
  * Saves credentials both to `/home/wifi.cfg` (`ssid=...`, `pass=...`, `auto=1`) and to the **ESP32-C6's own NVS flash** (`RPC_REQ_WIFI_SET_STORAGE = FLASH`), so remembered Wi-Fi networks survive power cycles even when running on a volatile RAM disk.
  * The background `wifid` kernel task automatically connects to the saved network at boot and reconnects if the link drops (unless manually disconnected by the user).

### Graphical Wi-Fi App (`wifi` / `run wifi` / `W` in Filer)
* **Top Status Window**: Displays connection state (`[CONNECTED]` / `[DISCONNECTED]`), current SSID, channel, RSSI dBm, IPv4 address, gateway, MAC address, remembered SSID, and `[Auto-Connect: ON/OFF]`.
* **Interactive Network List**: Lists scanned 2.4 GHz APs sorted by signal strength with color-coded 4-bar graphical signal meters, security badges (`OPEN`, `WPA2`, `WPA3`, `WPA2/3`), channel numbers, and `[CONNECTED]` / `[SAVED]` badges.
* **Keyboard Controls**:
  * `↑` / `↓` / `PgUp` / `PgDn`: Select a Wi-Fi network from the list
  * `Enter` / `C`: Connect to the selected network (pre-fills saved password if matching, or opens the graphical password input dialog for secured networks)
  * `S`: Rescan nearby Wi-Fi networks
  * `D`: Open the **Graphical Disconnect Dialog** to disconnect from the current AP
  * `A`: Toggle automatic connection at boot (`ON` / `OFF`)
  * `X`: Forget saved Wi-Fi credentials
  * `Q` / `ESC`: Close the Wi-Fi manager

### Command-Line & GUI Wi-Fi Commands in Shell
```sh
wifi                  # Launch the full-screen graphical Wi-Fi configuration app
wifi gui              # Same as above
wifi disconnect gui   # Open the graphical disconnect confirmation dialog from the CLI
wifi disconnect       # Immediately disconnect from Wi-Fi via CLI
wifi status           # Print current Wi-Fi state, SSID, RSSI, IP, gateway, MAC, and saved profile
wifi scan             # Scan and print nearby Wi-Fi networks in a formatted table
wifi connect <SSID> [PASSWORD]  # Connect to an AP from the CLI and save credentials
wifi auto [on|off]    # Enable or disable automatic connection on boot
wifi forget           # Erase saved Wi-Fi credentials from /home/wifi.cfg and ESP32-C6 NVS
```

---

## Multi-Tab Graphical SSH-2 Client (`ssh`)

Implemented in `user/ssh/ssh_app.c` (RFC 4253 / 4252 / 4254 / 8731):
* **Self-Contained Cryptography**: Built-in, zero-dependency implementations of **X25519** (`curve25519-sha256` & `curve25519-sha256@libssh.org`), **AES-128-CTR** (`aes128-ctr`), and **SHA-256 / HMAC-SHA-256** (`hmac-sha2-256`).
* **Authentication & Saved Host/Password Manager (`/home/ssh_hosts.cfg`)**:
  * Supports both `"password"` and `"keyboard-interactive"` (PAM) authentication, displaying the server's SSH-2 banner, host key type, and `SHA256` fingerprint upon connection.
  * Automatically persists connected hosts (`user@host:port`) and their passwords in `/home/ssh_hosts.cfg` (up to 16 profiles).
  * Launching `ssh` with no arguments (or `ssh gui`, `run ssh`, or pressing `S` in Filer) opens the **Graphical SSH Host & Tab Manager**, where you can select a saved host with `↑`/`↓` and press `Enter` to connect in the current tab or `T` to open in a new tab—without re-entering the password.
  * Supports adding (`N`), editing (`E`), deleting (`D`), and revealing/masking passwords (`P`) directly in the GUI.
* **4 Simultaneous SSH Tabs & Terminal Scrollback History**:
  * Supports up to **4 concurrent SSH-2 sessions (`Tab 1` .. `Tab 4`)** backed by 4 independent TCP sockets (`wifi_tcp_*_sock`) and PSRAM-allocated VT100/ANSI color terminal buffers.
  * Switch between active tabs at any time using `Ctrl-T`, `F1`–`F4`, `Ctrl-B 1`–`4`, `Ctrl-B n`/`p`, or via the Host Manager GUI (`Ctrl-O`).
  * Each tab maintains a **256-line scrollback ring buffer** in PSRAM. Press `PgUp` or `Ctrl-U` (or `Ctrl-B u` / `Ctrl-B [`) to scroll through past terminal output (`↑`/`↓` for line scroll, `PgUp`/`PgDn` for page scroll, `Home`/`End` for top/bottom, `Enter`/`Esc` to return to live view).

```sh
ssh                                          # Open Multi-Tab Graphical SSH Client & Host Manager
ssh user@192.168.1.10                        # Connect from CLI (uses saved password if registered)
ssh user@192.168.1.10 -pw secret uname -a    # Execute a single remote command & save profile
ssh hosts                                    # List saved SSH hosts and password status
ssh add user@192.168.1.10:22 secret          # Register/update a saved SSH host & password
ssh del 1                                    # Delete saved SSH profile #1
```

---

## Japanese Input Method (FEP)

Every keyboard key goes through `drivers/ime` before it reaches the console, so anything that reads
`kconsole_getc()` (the shell, `edit`, `ssh`, the browser's address bar ...) receives UTF-8 text.

| Key | Action |
|---|---|
| `Ctrl+Space` / `Alt`+backtick | Japanese input on / off (a green あ / ア badge shows it is on) |
| letters | romaji → hiragana (`nn`, `n'`, doubled consonant = っ, `xtu`, `-` = ー, `, . [ ] /` = 、。「」・) |
| `Space` | convert to kanji; again: candidate list (`Space`/`↓` next, `↑` previous, `1`–`9` pick) |
| `←` `→` / `Shift+←` `Shift+→` | next phrase / shorten or lengthen the current phrase while converting |
| `Enter` | commit · `Esc` / `Backspace` while converting: back to the reading |
| `F6` `F7` `F9` `F10` | respell as hiragana / katakana / full-width Latin / half-width Latin (`F6`/`F7` with nothing typed: switch the input mode) |

* **Conversion** (`conv.c`) cuts the reading into phrases by dynamic programming: a dictionary word, plain or conjugating
  (SKK okuri-ari entries, checked against the conjugation form the okurigana spells), followed by particles / endings
  that stay kana. Choices are remembered for the session (`ime_dict_learn`).
* **Screen**: console programs get an overlay at the text cursor (`fbcon_overlay_*`); programs that own the display
  (`fbcon_pause`: filer, ssh, web) get the same UI as a two-line bar at the bottom, restored afterwards.
* **Dictionary**: a small one is built in (`tools/ime/mini.skk`). For real use put an SKK dictionary in flash once:
  `tools/ime_flash_dict.sh -p /dev/cu.usbmodemXXXX` (downloads SKK-JISYO.L, GPL-2.0+, and writes it to the `imedict` partition
  at `0x400000`; it is loaded into PSRAM in the background after boot). `partitions_tab5.csv` now gives the app 3.9 MB.
* **Tests** run on the host: `tools/ime/host_test.sh [dict.bin]`.

## Known Limitations / TODO

* Single-core execution (`CPU1` is not started).
* Task stacks default to 4 KB (`kmalloc_internal`) without hardware stack guard pages (heavy tasks like `shell`, `wifid`, and `app` use 6–8 KB stacks via `task_create_stack`).
* Do not use floating-point / PIE coprocessors across tasks on ESP32-S3 (lazy FPU context saving is not implemented).
* Strict priority scheduling without mutex priority inheritance.
* Japanese input: `edit` still moves its cursor per byte (it inserts UTF-8 but cannot step over a multi-byte character); choices are not saved across reboots; only the Tab5 builds the IME.
* Tab5 console touch auto-detects GT911 (`0x14`, ILI9881C units) or ST712x (`0x55`, ST7121/ST7123 units).
  The ST712x protocol uses big-endian coordinates and per-slot valid bits; the GT911 ready/acknowledge
  protocol is retained. Both use the system I2C bus even with a Tab5 Keyboard on Ext.Port1.
  Use `osk on` / `osk off`, `osk debug`, and `osk map 0`–`3` to inspect and calibrate the console keyboard.
  The default portrait-to-landscape mapping is `(1279 - raw_y, raw_x)`; landscape-configured controllers use map 0.
  This does not add touch navigation to full-screen apps or implement the unsupported ST7123 LCD initialization.
  Driver regression tests (mock I2C, GT911 and ST712x): `sh tools/touch/host_test.sh`.
* RISC-V M5EX SDK build toolchain is planned for a future update.
