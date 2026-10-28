# M5OS lives in one IDF "component" only because IDF's build system wants
# one.  The real structure is the directory tree at the repository root.
set(M5OS_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

file(GLOB_RECURSE M5OS_SRCS
    ${M5OS_ROOT}/kernel/*.c   ${M5OS_ROOT}/kernel/*.S
    ${M5OS_ROOT}/drivers/*.c  ${M5OS_ROOT}/hal/*.c  ${M5OS_ROOT}/hal/*.S
    ${M5OS_ROOT}/fs/*.c       ${M5OS_ROOT}/user/*.c ${M5OS_ROOT}/apps/*.c)
# apps/m5ex/*.c and apps/sdk/*.c are SOURCES OF LOADABLE APPS, built separately by
# tools/m5exbuild.py with their own link layout; the firmware only embeds the generated images.c.
list(FILTER M5OS_SRCS EXCLUDE REGEX "/apps/sdk/|/apps/m5ex/[^i]")      # keep images.c / images_none.c (generated / stub)

# Board-specific sources: *_cores3.c, the chip directories hal/esp32s3 (Xtensa) and
# hal/esp32p4 (RISC-V), and the drivers that only some boards have.
if("$ENV{M5OS_BOARD}" STREQUAL "tab5")
    list(FILTER M5OS_SRCS EXCLUDE REGEX "_cores3\\.c$|/hal/esp32s3/|/drivers/(touch|button|sd)/|/drivers/lcd/(ili9342|display)\\.c$|/m5ex/images\\.c$")
    set(M5OS_BOARD_DEF M5OS_BOARD_TAB5=1)
    set(M5OS_ARCH_REQUIRES riscv esp_psram esp_mm esp_lcd usb)
else()
    list(FILTER M5OS_SRCS EXCLUDE REGEX "/hal/esp32p4/|/drivers/(usbkbd|tab5kbd)/|/drivers/lcd/display_tab5\\.c$|/m5ex/images_none\\.c$")
    set(M5OS_BOARD_DEF M5OS_BOARD_CORES3=1)
    set(M5OS_ARCH_REQUIRES xtensa)
endif()

# Developer switch: M5OS_SELFTEST=1 idf.py build   -> run the regression tests at boot instead of the shell
if("$ENV{M5OS_SELFTEST}" STREQUAL "1")
    set(M5OS_DEFS M5OS_BOOT_SELFTEST=1)
endif()

idf_component_register(
    SRCS ${M5OS_SRCS}
    INCLUDE_DIRS ${M5OS_ROOT}/kernel/include ${M5OS_ROOT}/hal
    # Only low-level, FreeRTOS-free IDF pieces are linked in explicitly.
    PRIV_REQUIRES soc hal esp_rom esp_hw_support ${M5OS_ARCH_REQUIRES})

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${M5OS_BOARD_DEF} ${M5OS_DEFS})

# THE hook that keeps FreeRTOS from ever running:
# startup.c calls esp_startup_start_app(); freertos/port/xtensa/port.c
# implements it as "create main task + vTaskStartScheduler()".  --wrap
# redirects that one call to __wrap_esp_startup_start_app() in
# kernel/boot/boot_hook.c.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_startup_start_app")
