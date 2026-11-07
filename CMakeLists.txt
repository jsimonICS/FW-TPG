cmake_minimum_required(VERSION 3.13)

# Builds apps/tpg out-of-tree against the wiliOGbsp submodule, so tpg's own
# history and this repo's submodule bumps never collide. Mirrors wiliOGbsp's
# own top-level CMakeLists.txt preamble -- see that file for the reasoning
# behind each setting; this only reproduces what apps/tpg (and the
# template_display it embeds) actually need.
set(PICO_BOARD freewili_og CACHE STRING "Board type" FORCE)
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/wiliOGbsp/bsp/boards")
set(PICO_STDIO_UART 0 CACHE BOOL "stdio over UART (forced off: UART0 is the link)" FORCE)

include(wiliOGbsp/pico_sdk_import.cmake)
project(tpg C CXX ASM)
set(CMAKE_C_STANDARD 11)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

pico_sdk_init()

execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE FWOG_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _fwog_git_rc)
if(NOT _fwog_git_rc EQUAL 0 OR FWOG_GIT_DESCRIBE STREQUAL "")
    set(FWOG_GIT_DESCRIBE "dev")
endif()

execute_process(
    COMMAND git log -1 --format=%ct
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE FWOG_GIT_COMMIT_TS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _fwog_ts_rc)
if(NOT _fwog_ts_rc EQUAL 0 OR NOT FWOG_GIT_COMMIT_TS MATCHES "^[0-9]+$")
    set(FWOG_GIT_COMMIT_TS 0)
endif()

add_subdirectory(wiliOGbsp/bsp)

add_subdirectory(wiliOGbsp/apps/bl)

# template_display is tpg_main's embedded display companion (tpg has none of
# its own) -- must be declared before apps/tpg.
add_subdirectory(wiliOGbsp/apps/template)
add_subdirectory(apps/tpg)
add_subdirectory(apps/btntest)
