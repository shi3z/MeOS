cmake_minimum_required(VERSION 3.16)

# Board selection: M5OS_BOARD=cardputer | tab5, or the default cores3.  Use tools/build.sh.
if("$ENV{M5OS_BOARD}" STREQUAL "cardputer")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.cardputer")
elseif("$ENV{M5OS_BOARD}" STREQUAL "tab5")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.tab5")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.cores3")
endif()

if("$ENV{M5OS_BOARD}" STREQUAL "tab5" AND "$ENV{M5OS_DOOM}" STREQUAL "1")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.doom")
    if("$ENV{M5OS_DOOM_RELEASE}" STREQUAL "1")
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.doom-release")
    endif()
endif()

if("$ENV{M5OS_QUAKE}" STREQUAL "1")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.doom" "sdkconfig.defaults.quake")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5os)

if("$ENV{M5OS_BOARD}" STREQUAL "tab5" AND "$ENV{M5OS_DOOM}" STREQUAL "1")
    # INSERT scripts must precede IDF sections.ld on the linker command line.
    target_link_options(m5os.elf BEFORE PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/components/doomengine/data.ld")
endif()

if("$ENV{M5OS_BOARD}" STREQUAL "tab5" AND "$ENV{M5OS_QUAKE}" STREQUAL "1")
    if("$ENV{M5OS_DOOM}" STREQUAL "1")
        message(FATAL_ERROR "Select one native engine per image")
    endif()
    target_link_options(m5os.elf BEFORE PRIVATE "-T${CMAKE_CURRENT_SOURCE_DIR}/components/quakeengine/data.ld")
endif()
