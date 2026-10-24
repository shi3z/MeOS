# M5OS lives in one IDF "component" only because IDF's build system wants
# one.  The real structure is the directory tree at the repository root.
set(M5OS_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

file(GLOB_RECURSE M5OS_SRCS
    ${M5OS_ROOT}/kernel/*.c   ${M5OS_ROOT}/kernel/*.S
    ${M5OS_ROOT}/drivers/*.c  ${M5OS_ROOT}/hal/*.c  ${M5OS_ROOT}/hal/*.S
    ${M5OS_ROOT}/fs/*.c       ${M5OS_ROOT}/user/*.c ${M5OS_ROOT}/apps/*.c
    ${M5OS_ROOT}/third_party/wasm3/*.c ${M5OS_ROOT}/third_party/quirc/*.c ${M5OS_ROOT}/third_party/fatfs/ff.c ${M5OS_ROOT}/third_party/fatfs/ffunicode.c)
# apps/m5ex/*.c and apps/sdk/*.c are SOURCES OF LOADABLE APPS, built separately by
# tools/m5exbuild.py with their own link layout; the firmware only embeds the generated images.c.
list(FILTER M5OS_SRCS EXCLUDE REGEX "/apps/sdk/|/apps/m5ex/[^i]|/apps/wasm/[^w]")
# keep images.c / images_none.c (generated / stub).  apps/wasm holds the sources of WebAssembly apps: only wasm_images.c is firmware.
set(M5OS_NO_WASM_RE "/third_party/wasm3/|/third_party/quirc/|/kernel/app/wasm_|/apps/wasm/wasm_images\\.c$|/user/wasmapps/")      # WebAssembly runs on the Tab5 only

# Board-specific sources: *_cores3.c / *_cardputer.c, the chip directories hal/esp32s3 (Xtensa) and
# hal/esp32p4 (RISC-V), and the drivers that only some boards have.
if("$ENV{M5OS_BOARD}" STREQUAL "cardputer")
    list(FILTER M5OS_SRCS EXCLUDE REGEX "_cores3\\.c$|/hal/esp32p4/|${M5OS_NO_WASM_RE}|/third_party/fatfs/|/fs/fat/|/drivers/sd/sd_tab5\\.c$|/drivers/(touch|button|usbkbd|tab5kbd|ime)/|/drivers/lcd/(ili9342|display_tab5)\\.c$|/drivers/camera/camera_tab5\\.c$|/drivers/wifi/wifi_tab5\\.c$|/m5ex/images_none\\.c$")
    set(M5OS_BOARD_DEF M5OS_BOARD_CARDPUTER=1)
    set(M5OS_ARCH_REQUIRES xtensa)
elseif("$ENV{M5OS_BOARD}" STREQUAL "tab5")
    list(FILTER M5OS_SRCS EXCLUDE REGEX "_cores3\\.c$|_cardputer\\.c$|/hal/esp32s3/|/drivers/(touch|button|keyboard)/|/drivers/sd/sd_spi\\.c$|/drivers/lcd/(ili9342|st7789|display)\\.c$|/drivers/camera/camera_stub\\.c$|/drivers/wifi/wifi_stub\\.c$|/m5ex/images\\.c$")
    set(M5OS_BOARD_DEF M5OS_BOARD_TAB5=1)
    set(M5OS_ARCH_REQUIRES riscv esp_psram esp_mm esp_lcd esp_driver_cam esp_driver_isp esp_driver_ledc usb)
else()
    list(FILTER M5OS_SRCS EXCLUDE REGEX "_cardputer\\.c$|/hal/esp32p4/|${M5OS_NO_WASM_RE}|/third_party/fatfs/|/fs/fat/|/drivers/sd/sd_tab5\\.c$|/drivers/(keyboard|usbkbd|tab5kbd|ime)/|/drivers/lcd/(st7789|display_tab5)\\.c$|/drivers/camera/camera_tab5\\.c$|/drivers/wifi/wifi_tab5\\.c$|/m5ex/images_none\\.c$")
    set(M5OS_BOARD_DEF M5OS_BOARD_CORES3=1)
    set(M5OS_ARCH_REQUIRES xtensa)
endif()

if(NOT "$ENV{M5OS_BOARD}" STREQUAL "tab5")
    list(FILTER M5OS_SRCS EXCLUDE REGEX "/user/player/mp3_stream\\.c$")
endif()

# Developer switch: M5OS_SELFTEST=1 idf.py build   -> run the regression tests at boot instead of the shell
if("$ENV{M5OS_SELFTEST}" STREQUAL "1")
    set(M5OS_DEFS M5OS_BOOT_SELFTEST=1)
endif()

if("$ENV{M5OS_WATCHDOG}" STREQUAL "1")
    if(NOT "$ENV{M5OS_BOARD}" STREQUAL "tab5")
        message(FATAL_ERROR "The opt-in hardware watchdog currently requires Tab5")
    endif()
    list(APPEND M5OS_DEFS M5OS_WATCHDOG_ENABLED=1)
endif()

idf_component_register(
    SRCS ${M5OS_SRCS}
    INCLUDE_DIRS ${M5OS_ROOT}/kernel/include ${M5OS_ROOT}/hal ${M5OS_ROOT}/third_party/wasm3 ${M5OS_ROOT}/third_party/quirc ${M5OS_ROOT}/third_party/fatfs
    # Only low-level, FreeRTOS-free IDF pieces are linked in explicitly.
    PRIV_REQUIRES soc hal esp_rom esp_hw_support ${M5OS_ARCH_REQUIRES})

execute_process(COMMAND git -C "${M5OS_ROOT}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE M5OS_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT M5OS_BUILD_ID)
    set(M5OS_BUILD_ID unknown)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${M5OS_BOARD_DEF} ${M5OS_DEFS}
    M5OS_BUILD_ID="${M5OS_BUILD_ID}")

# wasm3 (third_party/wasm3, MIT): the interpreter behind .wasm apps.  Vendored as is except for the allocator hook in
# m3_core.c; its own warnings are not ours to fix.  Optimised for speed: it is the hot loop of every WebAssembly app.
file(GLOB M5OS_WASM3_SRCS ${M5OS_ROOT}/third_party/wasm3/*.c)
set_source_files_properties(${M5OS_WASM3_SRCS} PROPERTIES COMPILE_OPTIONS "-w;-O3" COMPILE_DEFINITIONS "M5OS_WASM_ALLOC=1")

# THE hook that keeps FreeRTOS from ever running:
# startup.c calls esp_startup_start_app(); freertos/port/xtensa/port.c
# implements it as "create main task + vTaskStartScheduler()".  --wrap
# redirects that one call to __wrap_esp_startup_start_app() in
# kernel/boot/boot_hook.c.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_startup_start_app")

# quirc (third_party/quirc, ISC): QR-code reader for installing apps from the camera.  Vendored as is except for its allocator.
file(GLOB M5OS_QUIRC_SRCS ${M5OS_ROOT}/third_party/quirc/*.c ${M5OS_ROOT}/third_party/fatfs/ff.c ${M5OS_ROOT}/third_party/fatfs/ffunicode.c)
set_source_files_properties(${M5OS_QUIRC_SRCS} PROPERTIES COMPILE_OPTIONS "-w;-O2")

# MP3 decoding is never run by the 4KB audio callback task. Scalar floating
# point state is saved by the P4 scheduler/tick context; S3 playback is disabled.
if("$ENV{M5OS_BOARD}" STREQUAL "tab5")
    set_source_files_properties(${M5OS_ROOT}/user/player/mp3_stream.c
        PROPERTIES COMPILE_OPTIONS "-O2;-fno-tree-vectorize;-march=rv32imafc_zicsr_zifencei")
endif()
