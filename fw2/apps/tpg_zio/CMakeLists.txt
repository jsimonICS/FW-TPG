add_executable(tpg_zio main.c ${CMAKE_CURRENT_LIST_DIR}/../../../apps/tpg/common/tpg_patterns.c)

# The nine ZoomIO programs are generated at build time, so the app always
# carries the generator's current cycle model.
set(TPG_ZIO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${TPG_ZIO_GEN_DIR}/tpg_zio_scripts.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/gen_tpg_zio.py
            --out-dir ${TPG_ZIO_GEN_DIR}/zio --header ${TPG_ZIO_GEN_DIR}/tpg_zio_scripts.h
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/gen_tpg_zio.py
    COMMENT "Generating TPG ZoomIO programs"
    VERBATIM)
add_custom_target(tpg_zio_scripts DEPENDS ${TPG_ZIO_GEN_DIR}/tpg_zio_scripts.h)
add_dependencies(tpg_zio tpg_zio_scripts)

# apps/tpg for "common/tpg_patterns.h", shared with the OG build.
target_include_directories(tpg_zio PRIVATE ${TPG_ZIO_GEN_DIR}
                           ${CMAKE_CURRENT_LIST_DIR}/../../../apps/tpg)
target_link_libraries(tpg_zio freewili2_bsp onewili_fwgui)
# SENSORS carries the I/O expander that gates the header VIO rail. The FPGA
# zone run_zio needs has no POWER_ZONES name; main.c holds it with picpwr.
fw2_display_app(tpg_zio
    POWER_ZONES SENSORS DISPLAY SDCARD
    VERSION 001
    DESCRIPTION "NTSC test pattern generator: ZoomIO on the main CPU drives RGB + CSYNC on the header")
