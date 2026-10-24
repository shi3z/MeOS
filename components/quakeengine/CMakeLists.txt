if(NOT "$ENV{M5OS_BOARD}" STREQUAL "tab5" OR NOT "$ENV{M5OS_QUAKE}" STREQUAL "1")
    idf_component_register()
    return()
endif()
set(QROOT ${CMAKE_CURRENT_LIST_DIR}/../../third_party/quakegeneric/source)
idf_component_register(SRCS
    ${QROOT}/chase.c
    ${QROOT}/cl_demo.c
    ${QROOT}/cl_input.c
    ${QROOT}/cl_main.c
    ${QROOT}/cl_parse.c
    ${QROOT}/cl_tent.c
    ${QROOT}/cmd.c
    ${QROOT}/common.c
    ${QROOT}/console.c
    ${QROOT}/crc.c
    ${QROOT}/cvar.c
    ${QROOT}/d_edge.c
    ${QROOT}/d_fill.c
    ${QROOT}/d_init.c
    ${QROOT}/d_modech.c
    ${QROOT}/d_part.c
    ${QROOT}/d_polyse.c
    ${QROOT}/d_scan.c
    ${QROOT}/d_sky.c
    ${QROOT}/d_sprite.c
    ${QROOT}/d_surf.c
    ${QROOT}/d_vars.c
    ${QROOT}/d_zpoint.c
    ${QROOT}/draw.c
    ${QROOT}/host_cmd.c
    ${QROOT}/host.c
    ${QROOT}/in_null.c
    ${QROOT}/keys.c
    ${QROOT}/mathlib.c
    ${QROOT}/menu.c
    ${QROOT}/model.c
    ${QROOT}/net_loop.c
    ${QROOT}/net_main.c
    ${QROOT}/net_vcr.c
    ${QROOT}/nonintel.c
    ${QROOT}/pr_cmds.c
    ${QROOT}/pr_edict.c
    ${QROOT}/pr_exec.c
    ${QROOT}/r_aclip.c
    ${QROOT}/r_alias.c
    ${QROOT}/r_bsp.c
    ${QROOT}/r_draw.c
    ${QROOT}/r_edge.c
    ${QROOT}/r_efrag.c
    ${QROOT}/r_light.c
    ${QROOT}/r_main.c
    ${QROOT}/r_misc.c
    ${QROOT}/r_part.c
    ${QROOT}/r_sky.c
    ${QROOT}/r_sprite.c
    ${QROOT}/r_surf.c
    ${QROOT}/r_vars.c
    ${QROOT}/sbar.c
    ${QROOT}/screen.c
    ${QROOT}/snd_dma.c
    ${QROOT}/snd_mem.c
    ${QROOT}/snd_mix.c
    ${QROOT}/sv_main.c
    ${QROOT}/sv_move.c
    ${QROOT}/sv_phys.c
    ${QROOT}/sv_user.c
    ${QROOT}/vid_null.c
    ${QROOT}/view.c
    ${QROOT}/wad.c
    ${QROOT}/world.c
    ${QROOT}/zone.c
    ${QROOT}/net_none.c
    sys_meos.c sound_meos.c
    INCLUDE_DIRS ${QROOT} ${CMAKE_CURRENT_LIST_DIR}/../../kernel/include
    LDFRAGMENTS quake.lf)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fno-tree-vectorize -fno-strict-aliasing -fstack-usage -fno-builtin -msmall-data-limit=0 -w
    -include ${CMAKE_CURRENT_LIST_DIR}/compat.h)
target_compile_definitions(${COMPONENT_LIB} PRIVATE MEOS_QUAKE_PROFILE=1)
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/isolate_data.py
        ${CMAKE_OBJDUMP} ${CMAKE_OBJCOPY} $<TARGET_FILE:${COMPONENT_LIB}>
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/../../tools/quake/check_stack.py
        ${CMAKE_CURRENT_BINARY_DIR}
    VERBATIM)
