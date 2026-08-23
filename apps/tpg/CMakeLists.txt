add_executable(tpg_main main/main.c main/video_pattern.c)
target_link_libraries(tpg_main fwog_main_bsp)
pico_generate_pio_header(tpg_main ${CMAKE_CURRENT_LIST_DIR}/main/tpg_video.pio)
# Carries the display firmware inside this UF2. This app has no display
# companion of its own (all the real work is on the main CPU, driving the
# breakout header) -- reuse the template's minimal display skeleton.
fwog_embed_display_image(tpg_main template_display)
fwog_main_app(tpg_main
    VERSION 001
    DESCRIPTION "15.734 kHz RGB + composite-sync test pattern generator, driven from the breakout header via PIO (UNVERIFIED on hardware)")
