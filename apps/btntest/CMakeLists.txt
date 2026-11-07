add_executable(btntest_display display/main.c)
target_link_libraries(btntest_display fwog_display_bsp)
fwog_display_app(btntest_display
    VERSION 001
    DESCRIPTION "Button test: shows the pressed button's color and name, plus which are held")

add_executable(btntest_main main/main.c)
target_link_libraries(btntest_main fwog_main_bsp)
# Carries btntest_display (declared above, same folder) inside this UF2.
fwog_embed_display_image(btntest_main)
fwog_main_app(btntest_main
    VERSION 001
    DESCRIPTION "Button test, main half: brings the display up, then a heartbeat")
