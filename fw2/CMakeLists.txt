cmake_minimum_required(VERSION 3.13)

# FreeWili 2 apps, built out-of-tree against the wilibsp submodule. Kept in a
# CMake project of its own because the repo-root CMakeLists.txt selects the
# FreeWili OG board, and one configure can only target one board. Follows
# wilibsp/docs/app-project-setup.md: add wilibsp/bsp (never the wilibsp root,
# which registers every BSP example) plus the display-CPU OneWili library.

# RP2350B board header. NEVER pass -DPICO_BOARD on the command line -- it
# overrides this and reverts to the wrong chip (wilibsp/AGENTS.md invariant 1).
set(PICO_BOARD freewili2 CACHE STRING "Board type")
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/../wilibsp/bsp/boards")

# agentio (input injection + screen capture) stays on, as in wilibsp, so
# `fw screenshot` / `fw press` work against these apps on the bench.
option(FW2_AGENTIO "Build the agentio remote-control/capture harness" ON)

set(PICO_PIO_USB_PATH "${CMAKE_CURRENT_LIST_DIR}/../wilibsp/bsp/third_party/Pico-PIO-USB")
include(../wilibsp/pico_sdk_import.cmake)
project(tpg_fw2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
pico_sdk_init()

add_subdirectory(../wilibsp/bsp wilibsp_bsp)
add_subdirectory(../wilibsp/libs/onewili/wilibsp onewili)

add_subdirectory(apps/zio_probe)
add_subdirectory(apps/tpg_zio)
add_subdirectory(apps/tpg_fpga)
add_subdirectory(apps/exercise_timer)
