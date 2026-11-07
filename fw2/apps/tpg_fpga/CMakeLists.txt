add_executable(tpg_fpga main.c ${CMAKE_CURRENT_LIST_DIR}/../../../apps/tpg/common/tpg_patterns.c)

# The FPGA design (fpga/tpg) is built by wilifpga and embedded at build time,
# so the app always carries the current gateware. Needs the wilifpga checkout
# at the repo root and its toolchain (wilifpga/tools/fwfpga toolchain install).
set(TPG_REPO ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(TPG_FPGA_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${TPG_FPGA_GEN_DIR}/tpg_fwfpga.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/gen_fwfpga_header.py
            --wilifpga ${TPG_REPO}/wilifpga --design-src ${TPG_REPO}/fpga/tpg
            --name tpg --out ${TPG_FPGA_GEN_DIR}/tpg_fwfpga.h
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/gen_fwfpga_header.py
            ${TPG_REPO}/fpga/tpg/user_core.v ${TPG_REPO}/fpga/tpg/tpg_text_rom.v
            ${TPG_REPO}/fpga/tpg/design.toml
    COMMENT "Building the tpg FPGA design (wilifpga) and embedding it"
    VERBATIM)
add_custom_target(tpg_fpga_gateware DEPENDS ${TPG_FPGA_GEN_DIR}/tpg_fwfpga.h)
add_dependencies(tpg_fpga tpg_fpga_gateware)

# apps/tpg for "common/tpg_patterns.h", shared with the OG build.
target_include_directories(tpg_fpga PRIVATE ${TPG_FPGA_GEN_DIR} ${TPG_REPO}/apps/tpg)
target_link_libraries(tpg_fpga freewili2_bsp onewili_fwgui)
# SENSORS carries the I/O expander that gates the header VIO rail. The FPGA
# zone has no POWER_ZONES name; main.c holds it with picpwr.
fw2_display_app(tpg_fpga
    POWER_ZONES SENSORS DISPLAY SDCARD
    VERSION 001
    DESCRIPTION "NTSC test pattern generator: the iCE40 FPGA drives RGB + CSYNC on the header")
