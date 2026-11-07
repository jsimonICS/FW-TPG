add_executable(exercise_timer main.c)
target_link_libraries(exercise_timer freewili2_bsp)
# Display, touch and buttons only: no MAIN link, no header GPIO.
fw2_display_app(exercise_timer
    POWER_ZONES DISPLAY
    VERSION 001
    DESCRIPTION "Exercise timer: flex and rest countdowns with a rep counter, all editable on screen")
