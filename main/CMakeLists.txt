# M5OS lives in one IDF "component" only because IDF's build system wants
# one.  The real structure is the directory tree at the repository root.
set(M5OS_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(M5OS_GLOB_OPTIONS CONFIGURE_DEPENDS)
endif()
file(GLOB_RECURSE M5OS_SRCS ${M5OS_GLOB_OPTIONS}
    ${M5OS_ROOT}/kernel/*.c   ${M5OS_ROOT}/kernel/*.S
    ${M5OS_ROOT}/drivers/*.c  ${M5OS_ROOT}/hal/*.c  ${M5OS_ROOT}/hal/*.S
    ${M5OS_ROOT}/fs/*.c       ${M5OS_ROOT}/user/*.c ${M5OS_ROOT}/apps/*.c
    ${M5OS_ROOT}/third_party/ubasic/*.c ${M5OS_ROOT}/third_party/wasm3/*.c ${M5OS_ROOT}/third_party/quirc/*.c ${M5OS_ROOT}/third_party/fatfs/ff.c ${M5OS_ROOT}/third_party/fatfs/ffunicode.c ${M5OS_ROOT}/third_party/littlefs/lfs.c ${M5OS_ROOT}/third_party/littlefs/lfs_util.c)
# apps/m5ex/*.c and apps/sdk/*.c are SOURCES OF LOADABLE APPS, built separately by
# tools/m5exbuild.py with their own link layout; the firmware only embeds the generated images.c.
list(FILTER M5OS_SRCS EXCLUDE REGEX "/apps/sdk/|/apps/m5ex/[^i]|/apps/wasm/[^w]")
# keep images.c / images_none.c (generated / stub).  apps/wasm holds the sources of WebAssembly apps: only wasm_images.c is firmware.
set(M5OS_NO_WASM_RE "/third_party/wasm3/|/third_party/quirc/|/kernel/app/wasm_|/apps/wasm/wasm_images\\.c$|/user/wasmapps/")      # WebAssembly runs on the Tab5 only

# Board-specific sources: *_cores3.c / *_cardputer.c, the chip directories hal/esp32s3 (Xtensa) and
# hal/esp32p4 (RISC-V), and the drivers that only some boards have.
if("$ENV{M5OS_BOARD}" STREQUAL "cardputer")
    list(FILTER M5OS_SRCS EXCLUDE REGEX "_cores3\\.c$|/hal/esp32p4/|${M5OS_NO_WASM_RE}|/third_party/fatfs/|/fs/fat/|/drivers/sd/(sd_tab5|sdmmc_tab5)\\.c$|/drivers/(touch|button|usbkbd|tab5kbd|ime)/|/drivers/lcd/(ili9342|display_tab5)\\.c$|/drivers/camera/camera_tab5\\.c$|/drivers/wifi/wifi_tab5\\.c$|/m5ex/images_none\\.c$")
    set(M5OS_BOARD_DEF M5OS_BOARD_CARDPUTER=1)
    set(M5OS_ARCH_REQUIRES xtensa)
elseif("$ENV{M5OS_BOARD}" STREQUAL "tab5")
    if("$ENV{M5OS_SD_SPI}" STREQUAL "1")
        list(FILTER M5OS_SRCS EXCLUDE REGEX "_cores3\\.c$|_cardputer\\.c$|/hal/esp32s3/|/drivers/(touch|button|keyboard)/|/drivers/sd/(sd_spi|sdmmc_tab5)\\.c$|/drivers/lcd/(ili9342|st7789|display)\\.c$|/drivers/camera/camera_stub\\.c$|/drivers/wifi/wifi_stub\\.c$|/m5ex/images\\.c$")
    else()
        list(FILTER M5OS_SRCS EXCLUDE REGEX "_cores3\\.c$|_cardputer\\.c$|/hal/esp32s3/|/drivers/(touch|button|keyboard)/|/drivers/sd/(sd_spi|sd_tab5)\\.c$|/drivers/lcd/(ili9342|st7789|display)\\.c$|/drivers/camera/camera_stub\\.c$|/drivers/wifi/wifi_stub\\.c$|/m5ex/images\\.c$")
    endif()
    set(M5OS_BOARD_DEF M5OS_BOARD_TAB5=1)
    set(M5OS_ARCH_REQUIRES riscv esp_psram esp_mm esp_lcd esp_driver_cam esp_driver_isp esp_driver_ledc usb)
    if("$ENV{M5OS_QUAKE}" STREQUAL "1")
        list(APPEND M5OS_ARCH_REQUIRES quakeengine)
        list(APPEND M5OS_BOARD_DEF M5OS_QUAKE=1)
    endif()
    if("$ENV{M5OS_DOOM}" STREQUAL "1")
        list(APPEND M5OS_ARCH_REQUIRES doomengine)
        list(APPEND M5OS_BOARD_DEF M5OS_DOOM=1 DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)
    endif()
else()
    list(FILTER M5OS_SRCS EXCLUDE REGEX "_cardputer\\.c$|/hal/esp32p4/|${M5OS_NO_WASM_RE}|/third_party/fatfs/|/fs/fat/|/drivers/sd/(sd_tab5|sdmmc_tab5)\\.c$|/drivers/(keyboard|usbkbd|tab5kbd|ime)/|/drivers/lcd/(st7789|display_tab5)\\.c$|/drivers/camera/camera_tab5\\.c$|/drivers/wifi/wifi_tab5\\.c$|/m5ex/images_none\\.c$")
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

if("$ENV{M5OS_BOARD}" STREQUAL "tab5" AND "$ENV{M5OS_DOOM}" STREQUAL "1")
    # The engine archive refers back to the private libc adapter in libmain.
    # Retain that adapter even when libmain is scanned before libdoomengine.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=doom_malloc")
    set_source_files_properties(${M5OS_ROOT}/apps/doom/doom.c ${M5OS_ROOT}/apps/doom/libc.c ${M5OS_ROOT}/apps/doom/input.c ${M5OS_ROOT}/apps/doom/sound.c ${M5OS_ROOT}/apps/doom/mixer.c ${M5OS_ROOT}/apps/doom/video.c ${M5OS_ROOT}/drivers/lcd/display_tab5.c
        ${M5OS_ROOT}/hal/common/i2c_bitbang.c ${M5OS_ROOT}/hal/common/gpio_basic.c
        PROPERTIES COMPILE_OPTIONS "-O2;-fno-builtin")
    set_source_files_properties(${M5OS_ROOT}/drivers/lcd/display_tab5.c
        PROPERTIES COMPILE_OPTIONS "-O3;-funroll-loops;-fno-builtin")
    target_include_directories(${COMPONENT_LIB} PRIVATE ${M5OS_ROOT}/third_party/doomgeneric)
    option(M5OS_DOOM_EMBED_WAD "Embed local game data (never for public candidates)" ON)
    set(DOOM_WAD_SIZE 0)
    set(DOOM_WAD_EMBEDDED false)
    if(M5OS_DOOM_EMBED_WAD)
        set(DOOM_WAD "$ENV{M5OS_DOOM_WAD}")
        if(NOT DOOM_WAD)
            set(DOOM_WAD "${M5OS_ROOT}/tools/doom/cache/doom1.wad")
        endif()
        if(DOOM_WAD AND EXISTS "${DOOM_WAD}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${DOOM_WAD}" "${M5OS_ROOT}/tools/doom/pack_wad.py")
            execute_process(COMMAND ${PYTHON} ${M5OS_ROOT}/tools/doom/pack_wad.py
                "${DOOM_WAD}" "${CMAKE_BINARY_DIR}/doom_wad.z" RESULT_VARIABLE DOOM_PACK_RESULT)
            if(NOT DOOM_PACK_RESULT EQUAL 0)
                message(FATAL_ERROR "Doom WAD validation/compression failed")
            endif()
            file(SIZE "${DOOM_WAD}" DOOM_WAD_SIZE)
            target_compile_definitions(${COMPONENT_LIB} PRIVATE DOOM_EMBED_RAW_SIZE=${DOOM_WAD_SIZE})
            target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/doom_wad.z" BINARY RENAME_TO doom_wad_z)
        endif()
    else()
        message(STATUS "Doom: game-data embedding explicitly disabled")
    endif()
    if(DOOM_WAD_SIZE GREATER 0)
        set(DOOM_WAD_EMBEDDED true)
    endif()
    if(M5OS_DOOM_EMBED_WAD)
        set(DOOM_EMBED_ALLOWED true)
    else()
        set(DOOM_EMBED_ALLOWED false)
    endif()
    execute_process(COMMAND git -C "${M5OS_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE DOOM_SOURCE_REV OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE DOOM_REV_RESULT)
    if(NOT DOOM_REV_RESULT EQUAL 0)
        set(DOOM_SOURCE_REV "unknown")
        if(EXISTS "${M5OS_ROOT}/SOURCE_REVISION")
            file(READ "${M5OS_ROOT}/SOURCE_REVISION" DOOM_SOURCE_REV)
            string(STRIP "${DOOM_SOURCE_REV}" DOOM_SOURCE_REV)
        endif()
    endif()
    configure_file("${M5OS_ROOT}/tools/doom/build-info.json.in"
        "${CMAKE_BINARY_DIR}/doom-build.json" @ONLY)

endif()

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
# littlefs is used through explicit static caches; never calls newlib allocation/logging.
file(GLOB M5OS_LITTLEFS_SRCS ${M5OS_ROOT}/third_party/littlefs/*.c)
set_source_files_properties(${M5OS_LITTLEFS_SRCS} PROPERTIES COMPILE_DEFINITIONS
    "LFS_THREADSAFE;LFS_NO_MALLOC;LFS_NO_DEBUG;LFS_NO_WARN;LFS_NO_ERROR;LFS_NO_ASSERT" COMPILE_OPTIONS "-O2")

# Quake uses the same rotated/indexed LCD hot path as Doom. Keep non-game builds
# unchanged; the default SDK debug choice must not de-optimize this game path.
if("$ENV{M5OS_QUAKE}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MEOS_QUAKE_PROFILE=1)
    set_source_files_properties(${M5OS_ROOT}/drivers/lcd/display_tab5.c ${M5OS_ROOT}/drivers/lcd/ppa_tab5.c
        PROPERTIES COMPILE_OPTIONS "-O3;-funroll-loops;-fno-builtin")
    set_source_files_properties(${M5OS_ROOT}/apps/quake/quake.c ${M5OS_ROOT}/apps/quake/libc.c
        ${M5OS_ROOT}/apps/quake/input.c ${M5OS_ROOT}/hal/common/i2c_bitbang.c
        ${M5OS_ROOT}/hal/common/gpio_basic.c
        PROPERTIES COMPILE_OPTIONS "-O2;-fno-builtin")
endif()
