add_executable(tpg_display display/main.c common/tpg_link_proto.c common/tpg_patterns.c)
target_include_directories(tpg_display PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(tpg_display fwog_display_bsp)
fwog_display_app(tpg_display
    VERSION 001
    DESCRIPTION "TPG pattern-select UI: buttons cycle patterns, name shown on the LCD")

add_executable(tpg_main main/main.c main/video_pattern.c common/tpg_link_proto.c common/tpg_patterns.c)
target_include_directories(tpg_main PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(tpg_main fwog_main_bsp)
pico_generate_pio_header(tpg_main ${CMAKE_CURRENT_LIST_DIR}/main/tpg_video.pio)
# Carries the display firmware inside this UF2. tpg_display (declared above,
# same folder) is the button/LCD pattern-select UI paired with this app --
# fwog_embed_display_image() with no second argument defaults to it.
fwog_embed_display_image(tpg_main)
fwog_main_app(tpg_main
    VERSION 002
    DESCRIPTION "15.734 kHz RGB + composite-sync TPG, 9 button-selectable patterns via the breakout header (UNVERIFIED on hardware)")
