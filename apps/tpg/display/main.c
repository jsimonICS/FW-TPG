/* TPG pattern-select UI: the OG's five front-panel buttons cycle through the
 * test patterns generated on the main CPU's breakout header (see
 * apps/tpg/main/video_pattern.c); the LCD shows the current pattern's name
 * as text. GREEN advances, YELLOW goes back, both wrapping. RED stays
 * reserved for the board's power-off hold (FWOG_POWER_DEFAULT() below) and
 * GRAY/BLUE are unused.
 *
 * This app owns the "current pattern" index: it's the side reading the
 * buttons, so it tracks the selection locally and sends the ABSOLUTE new
 * pattern id to main on every change (common/tpg_link_proto.h) -- not a
 * relative "cycle" step, so a single dropped link frame can't leave the two
 * CPUs permanently disagreeing about which pattern is live. */
#include "fwog_display.h"
#include "pico/stdlib.h"
#include "common/tpg_link_proto.h"
#include "common/tpg_patterns.h"

FWOG_POWER_DEFAULT();

static bool s_link_up;
static fwog_link_rx_t s_link_rx;
static tpg_pattern_id_t s_pattern = TPG_PATTERN_COLOR_BARS;

static void show_pattern(tpg_pattern_id_t p) {
    lcd_text_draw_padded(8, 8, tpg_pattern_name(p), 20, 2,
                         st7789_rgb565(255, 255, 255), st7789_rgb565(0, 0, 0));
}

static void send_pattern(tpg_pattern_id_t p) {
    if (!s_link_up) return;
    uint8_t buf[8];
    size_t n = tpg_link_proto_build_set_pattern(buf, sizeof buf, (uint8_t)p);
    if (n) (void)fwog_link_uart_send_frame(buf, n);
}

int main(void) {
    board_init();

    /* The bootloader deinits the link before jumping here (bl_jump.c), so
       the app must bring it back up itself -- see bench_display's identical
       comment at this call site. Brought up before anything slow (the LCD
       init below) because main's claim_breakout_pins_as_outputs() sends its
       CONFIG_ENABLE/SET_DIRS requests right after releasing us, and only
       this app can answer them (io_expander/ioexp_link.h). */
    s_link_up = fwog_link_uart_init(FWOG_LINK_BAUD);
    fwog_link_rx_init(&s_link_rx);

    fwog_buttons_init();

    /* Bounded, not unbounded -- st7789_init_begin()/step() can leave
       ST7789_INIT_IDLE forever if clk_peri never reaches the panel rate, and
       there is no watchdog on this CPU to recover from a spin. 500 ms
       comfortably covers the ~125 ms a healthy init needs (bench_display's
       figure for the same sequence). */
    st7789_init_begin();
    const absolute_time_t lcd_deadline = make_timeout_time_ms(500);
    while (!st7789_ready() && !time_reached(lcd_deadline)) st7789_init_step();
    const bool lcd_ready = st7789_ready();
    if (lcd_ready) {
        st7789_clear(0x0000u);
        st7789_dma_wait();
        board_backlight(255);
        show_pattern(s_pattern);
    }

    DIAG("[tpg_display] ready; link=%s lcd=%s\n",
         s_link_up ? "ok" : "FAILED", lcd_ready ? "ok" : "FAILED");

    absolute_time_t next_hb = make_timeout_time_ms(2000);
    while (true) {
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        const fwog_buttons_t buttons = fwog_power_poll(now).buttons;

        tpg_pattern_id_t next = s_pattern;
        if (buttons.pressed & FWOG_BTN_BIT(FWOG_BTN_GREEN)) {
            next = (tpg_pattern_id_t)((s_pattern + 1u) % TPG_PATTERN_COUNT);
        } else if (buttons.pressed & FWOG_BTN_BIT(FWOG_BTN_YELLOW)) {
            next = (tpg_pattern_id_t)((s_pattern + TPG_PATTERN_COUNT - 1u) %
                                      TPG_PATTERN_COUNT);
        }
        if (next != s_pattern) {
            s_pattern = next;
            if (lcd_ready) show_pattern(s_pattern);
            send_pattern(s_pattern);
        }

        /* Service main's breakout-direction requests; nothing else is
           expected from main. */
        uint8_t b;
        while (s_link_up && fwog_link_uart_read(&b)) {
            size_t len = 0u;
            if (fwog_link_rx_byte(&s_link_rx, b, &len))
                (void)fwog_ioexp_link_handle(s_link_rx.buf, len);
        }

        if (time_reached(next_hb)) {
            next_hb = make_timeout_time_ms(2000);
            DIAG("[tpg_display] alive, pattern=%s\n", tpg_pattern_name(s_pattern));
        }
        sleep_ms(2);
    }
}
