/* Button test, main half: nothing app-specific happens here. It exists
 * because only a main UF2 can carry a display app onto the board -- this
 * brings the display up (pushing btntest_display if it differs), then kicks
 * the watchdog with a heartbeat. Same shape as wiliOGbsp's template_main. */
#include "fwog_main.h"
#include "pico/stdlib.h"

FWOG_WATCHDOG_DEFAULT();

int main(void) {
    board_init();

    /* Performs board_release_display() itself -- do not call it separately. */
    fwog_display_result_t d = fwog_display_update_run();

    /* Repeated for 10 s: the handshake finishes before USB CDC enumerates,
       so a single print would be lost (see template_main). */
    const absolute_time_t announce_until = make_timeout_time_ms(10000);

    while (true) {
        board_watchdog_kick();
        if (!time_reached(announce_until)) {
            DIAG("[btntest_main] display: %s\n", fwog_display_result_text(d));
        } else {
            DIAG("[btntest_main] alive\n");
        }
        sleep_ms(500);
    }
}
