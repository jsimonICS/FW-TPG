add_executable(zio_probe main.c)
target_link_libraries(zio_probe freewili2_bsp onewili_fwgui)
# SENSORS carries the I/O expander that gates the header VIO rail. The FPGA
# zone run_zio needs has no POWER_ZONES name; main.c holds it with picpwr.
fw2_display_app(zio_probe
    POWER_ZONES SENSORS DISPLAY SDCARD
    VERSION 001
    DESCRIPTION "ZoomIO launch probe: runs a GPIO 25 square wave on the main CPU's RISC-V core1")
