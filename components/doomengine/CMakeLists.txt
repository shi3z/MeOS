if(NOT "$ENV{M5OS_BOARD}" STREQUAL "tab5" OR NOT "$ENV{M5OS_DOOM}" STREQUAL "1")
    idf_component_register()
    return()
endif()
set(DOOM_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/doomgeneric)
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
idf_component_register(SRCS ${DOOM_SOURCES} i_system_meos.c
    INCLUDE_DIRS ${DOOM_ROOT} ${CMAKE_CURRENT_LIST_DIR}/../../kernel/include
    LDFRAGMENTS doom.lf)
target_compile_definitions(${COMPONENT_LIB} PRIVATE DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-builtin -msmall-data-limit=0 -w
    -include ${CMAKE_CURRENT_LIST_DIR}/compat.h)
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/isolate_data.py
        ${CMAKE_OBJDUMP} ${CMAKE_OBJCOPY} $<TARGET_FILE:${COMPONENT_LIB}>
    VERBATIM)

# Preserve upstream source: replace only presentation/palette entry points.
set_source_files_properties(${DOOM_ROOT}/i_video.c PROPERTIES COMPILE_DEFINITIONS
    "I_FinishUpdate=doom_engine_finish_update;I_SetPalette=doom_engine_set_palette")
