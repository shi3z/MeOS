cmake_minimum_required(VERSION 3.16)

# Board selection: M5OS_BOARD=tab5, or the default cores3.  Use tools/build.sh.
if("$ENV{M5OS_BOARD}" STREQUAL "tab5")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.tab5")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.cores3")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5os)
