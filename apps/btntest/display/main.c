/* Button test: press any of the OG's five front-panel buttons and the LCD
 * names its color in a swatch of that color. A row of five indicators along
 * the bottom shows which buttons are held right now, so chords and stuck or
 * bouncy contacts are visible too. Every press and release is also echoed to
 * the USB console via DIAG.
 *
 * RED still belongs to the board's power-off hold (FWOG_POWER_DEFAULT()
 * below): a short press reports normally, but holding it for 6 s powers the
 * board off, with the countdown on the LED bar. */
#include "fwog_display.h"
#include "pico/stdlib.h"

FWOG_POWER_DEFAULT();

typedef struct {
    const char *name;
    uint8_t r, g, b;
    bool dark_text;      /* black text reads better on the light swatches */
} btn_color_t;

/* Indexed by fwog_btn_id_t. */
static const btn_color_t k_colors[FWOG_BTN_COUNT] = {
    [FWOG_BTN_GRAY]   = {"GRAY",   128, 128, 128, true},
    [FWOG_BTN_YELLOW] = {"YELLOW", 255, 220,   0, true},
    [FWOG_BTN_GREEN]  = {"GREEN",    0, 200,   0, true},
    [FWOG_BTN_BLUE]   = {"BLUE",     0,  80, 255, false},
    [FWOG_BTN_RED]    = {"RED",    230,   0,   0, false},
};

/* Layout on the 320x240 panel. */
#define TITLE_Y      8u
#define SWATCH_X     10u
#define SWATCH_Y     40u
#define SWATCH_W     300u
#define SWATCH_H     130u
#define NAME_SCALE   5u                               /* 30x40 px glyphs */
#define NAME_COLS    6u                               /* "YELLOW" */
#define IND_Y        190u
#define IND_W        52u
#define IND_H        40u
#define IND_GAP      ((ST7789_W - FWOG_BTN_COUNT * IND_W) / (FWOG_BTN_COUNT + 1u))

static uint16_t color_of(fwog_btn_id_t id) {
    return st7789_rgb565(k_colors[id].r, k_colors[id].g, k_colors[id].b);
}

static void show_swatch(fwog_btn_id_t id) {
    const uint16_t bg = color_of(id);
    const uint16_t fg = k_colors[id].dark_text ? st7789_rgb565(0, 0, 0)
                                               : st7789_rgb565(255, 255, 255);
    st7789_fill_rect(SWATCH_X, SWATCH_Y, SWATCH_W, SWATCH_H, bg);
    const unsigned text_w = lcd_text_width_px(NAME_COLS, NAME_SCALE);
    const uint16_t x = (uint16_t)(SWATCH_X + (SWATCH_W - text_w) / 2u);
    const uint16_t y = (uint16_t)(SWATCH_Y + (SWATCH_H - LCD_GLYPH_H * NAME_SCALE) / 2u);
    /* Centre short names inside the fixed six-column field. */
    char line[NAME_COLS + 1u];
    const char *name = k_colors[id].name;
    unsigned len = 0u;
    while (name[len] && len < NAME_COLS) len++;
    const unsigned pad = (NAME_COLS - len) / 2u;
    unsigned i = 0u;
    for (; i < pad; i++) line[i] = ' ';
    for (unsigned j = 0u; j < len; j++) line[i++] = name[j];
    line[i] = '\0';
    lcd_text_draw_padded(x, y, line, NAME_COLS, NAME_SCALE, fg, bg);
}

/* Held buttons draw solid in their color; released ones as a dim outline. */
static void show_indicators(uint8_t down) {
    const uint16_t off = st7789_rgb565(40, 40, 40);
    for (unsigned id = 0u; id < FWOG_BTN_COUNT; id++) {
        const uint16_t x = (uint16_t)(IND_GAP + id * (IND_W + IND_GAP));
        if (down & FWOG_BTN_BIT(id)) {
            st7789_fill_rect(x, IND_Y, IND_W, IND_H, color_of((fwog_btn_id_t)id));
        } else {
            st7789_fill_rect(x, IND_Y, IND_W, IND_H, off);
            st7789_fill_rect(x + 3u, IND_Y + 3u, IND_W - 6u, IND_H - 6u, 0x0000u);
        }
    }
}

int main(void) {
    board_init();
    fwog_buttons_init();

    /* Bounded for the same reason as tpg_display: a panel that never reaches
       ST7789_INIT_IDLE must not hang a CPU that has no watchdog. */
    st7789_init_begin();
    const absolute_time_t lcd_deadline = make_timeout_time_ms(500);
    while (!st7789_ready() && !time_reached(lcd_deadline)) st7789_init_step();
    const bool lcd_ready = st7789_ready();
    if (lcd_ready) {
        st7789_clear(0x0000u);
        lcd_text_draw((ST7789_W - lcd_text_width_px(11u, 2u)) / 2u, TITLE_Y,
                      "BUTTON TEST", 2u, st7789_rgb565(255, 255, 255), 0x0000u);
        lcd_text_draw((ST7789_W - lcd_text_width_px(18u, 2u)) / 2u,
                      SWATCH_Y + SWATCH_H / 2u - LCD_GLYPH_H,
                      "Press any button..", 2u, st7789_rgb565(160, 160, 160), 0x0000u);
        show_indicators(0u);
        st7789_dma_wait();
        board_backlight(255);
    }

    DIAG("[btntest_display] ready; lcd=%s\n", lcd_ready ? "ok" : "FAILED");

    uint8_t shown_down = 0u;
    absolute_time_t next_hb = make_timeout_time_ms(2000);
    while (true) {
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        /* Buttons come from the power poll, never a second
           fwog_buttons_poll(): that would steal edges from the red hold. */
        const fwog_buttons_t b = fwog_power_poll(now).buttons;

        for (unsigned id = 0u; id < FWOG_BTN_COUNT; id++) {
            if (b.pressed & FWOG_BTN_BIT(id)) {
                DIAG("[btntest_display] %s pressed\n", k_colors[id].name);
                if (lcd_ready) show_swatch((fwog_btn_id_t)id);
            }
            if (b.released & FWOG_BTN_BIT(id)) {
                DIAG("[btntest_display] %s released\n", k_colors[id].name);
            }
        }
        if (lcd_ready && b.down != shown_down) {
            shown_down = b.down;
            show_indicators(shown_down);
        }

        if (time_reached(next_hb)) {
            next_hb = make_timeout_time_ms(2000);
            DIAG("[btntest_display] alive, down=0x%02x\n", b.down);
        }
        sleep_ms(2);
    }
}
