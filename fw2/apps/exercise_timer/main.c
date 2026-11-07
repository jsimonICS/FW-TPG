/* exercise_timer — flex / rest interval timer with a rep counter, as a
 * display app for the FreeWili 2 (installed in /apps/misc).
 *
 * Each rep is a FLEX countdown (hold the position) followed by a REST
 * countdown. The rep counter counts down to zero as each flex completes; the
 * last rep ends without a rest. Defaults: 10 s flex, 10 s rest, 20 reps, all
 * editable on the setup screen. Settings are not saved, so every launch
 * starts from the defaults.
 *
 * The running screen is coloured by phase (FLEX orange, REST blue) and shows
 * the phase name, the seconds left in 140 px digits, the reps left and a
 * progress bar for the phase.
 *
 * Setup: touch a row to select it and touch - / + (hold to repeat, in steps
 * of 5 after 2 s), or use GREY next field, YELLOW -, GREEN start, BLUE +,
 * RED defaults. The nav pad selects (up/down) and adjusts (left/right); OK
 * starts. Running: YELLOW skips to the next phase, GREEN (or OK) pauses and
 * resumes, RED (or CANCEL) stops. Done: GREEN again, RED back to setup.
 *
 * Needs nothing from MAIN. HOLD HOME 5 S leaves the app. */
#include "fw2.h"
#include "platform/diag.h"
#include "display/font5x7.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

/* RGB565 -> the panel's wire (big-endian) order. */
#define BE(c) ((uint16_t)((((c) >> 8) & 0xFF) | (((c) & 0xFF) << 8)))

#define C_BLACK   BE(0x0000)
#define C_WHITE   BE(0xFFFF)
#define C_GREY    BE(0x8410)
#define C_DIM     BE(0xBDF7)
#define C_ROW     BE(0x2104)
#define C_FLEX    BE(0xD1C0)   /* orange */
#define C_FLEX_TR BE(0x6800)   /* its progress-bar track */
#define C_FLEX_HI BE(0xFC60)   /* its label on the dark setup rows */
#define C_REST    BE(0x0A1A)   /* blue */
#define C_REST_TR BE(0x0108)
#define C_REST_HI BE(0x4D7F)
#define C_DONE    BE(0x0420)   /* green */

/* ── settings ──────────────────────────────────────────────────────────── */
enum { F_FLEX, F_REST, F_REPS, F_COUNT };
static const int DEFAULTS[F_COUNT] = { 10, 10, 20 };
static const int MIN_V[F_COUNT] = { 1, 0, 1 };   /* REST 0 = no rest phase */
static const int MAX_V[F_COUNT] = { 300, 300, 200 };
static const char *const FIELD_NAME[F_COUNT] = { "FLEX", "REST", "REPS" };
static const char *const FIELD_UNIT[F_COUNT] = { "SECONDS", "SECONDS", "COUNT" };
static const uint16_t FIELD_COLOR[F_COUNT] = { C_FLEX_HI, C_REST_HI, C_WHITE };

static int setting[F_COUNT];
static int field;   /* selected setup row */

/* ── workout state ─────────────────────────────────────────────────────── */
typedef enum { SCR_SETUP, SCR_RUN, SCR_DONE } screen_t;
typedef enum { PH_FLEX, PH_REST } phase_t;

static screen_t screen;
static phase_t phase;
static bool paused;
static int reps_left;
static uint64_t phase_len_us;
static uint64_t phase_end_us;     /* while running */
static uint64_t paused_left_us;   /* while paused */

/* What the running screen shows, so a pass redraws only what changed;
 * -1 forces a redraw. */
static int shown_secs = -1, shown_len = -1, shown_bar = -1;

/* ── actions, and the coloured buttons that trigger them ──────────────── */
typedef enum {
    ACT_NONE, ACT_NEXT_FIELD, ACT_PREV_FIELD, ACT_DEC, ACT_INC, ACT_START,
    ACT_DEFAULTS, ACT_SKIP, ACT_PAUSE, ACT_STOP, ACT_AGAIN, ACT_SETUP
} action_t;

/* Grey, yellow, green, blue, red, per screen. */
static const action_t BAR[3][5] = {
    [SCR_SETUP] = { ACT_NEXT_FIELD, ACT_DEC, ACT_START, ACT_INC, ACT_DEFAULTS },
    [SCR_RUN]   = { ACT_NONE, ACT_SKIP, ACT_PAUSE, ACT_NONE, ACT_STOP },
    [SCR_DONE]  = { ACT_NONE, ACT_NONE, ACT_AGAIN, ACT_NONE, ACT_SETUP },
};

/* Button colours already in wire order, with the text that reads on each
 * (wilibsp AGENTS.md). */
static const uint16_t BTN_COLOR[5] = { 0x9AD6, 0x06FF, 0x0012, 0xF800, 0x0780 };
static const uint16_t BTN_TEXT[5] = { C_BLACK, C_BLACK, C_WHITE, C_WHITE, C_WHITE };

static const char *action_label(action_t a) {
    switch (a) {
    case ACT_NEXT_FIELD: return "NEXT";
    case ACT_DEC:        return "-";
    case ACT_INC:        return "+";
    case ACT_START:      return "START";
    case ACT_DEFAULTS:   return "DEFAULT";
    case ACT_SKIP:       return "SKIP";
    case ACT_PAUSE:      return paused ? "RESUME" : "PAUSE";
    case ACT_STOP:       return "STOP";
    case ACT_AGAIN:      return "AGAIN";
    case ACT_SETUP:      return "SETUP";
    default:             return NULL;
    }
}

/* ── drawing ───────────────────────────────────────────────────────────── */

/* Text in the 5x7 font at any integer scale (st7796_draw_text stops at 4),
 * 7 * scale px tall. Every pixel of the glyphs and of the gaps between them
 * is written once, foreground or background, so redrawing in place does not
 * flicker. Each run of same-coloured font pixels in a column is one fill. */
static int text_w(const char *s, int scale) {
    const int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

static void big_text(int x, int y, int scale, uint16_t fg, uint16_t bg, const char *s) {
    for (; *s; s++) {
        const char c = *s;
        const uint8_t *cols = (c >= FONT5X7_FIRST && c <= FONT5X7_LAST)
                                  ? font5x7[c - FONT5X7_FIRST]
                                  : font5x7[0];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7;) {
                const bool on = (cols[col] >> row) & 1;
                int end = row + 1;
                while (end < 7 && (bool)((cols[col] >> end) & 1) == on) end++;
                st7796_fill_rect(x + col * scale, y + row * scale, scale,
                                 (end - row) * scale, on ? fg : bg);
                row = end;
            }
        }
        x += 5 * scale;
        if (s[1]) {
            st7796_fill_rect(x, y, scale, 7 * scale, bg);
            x += scale;
        }
    }
}

static void text_centered(int cx, int y, int scale, uint16_t fg, uint16_t bg, const char *s) {
    big_text(cx - text_w(s, scale) / 2, y, scale, fg, bg, s);
}

/* Bottom-row labels, one fifth per button, directly above it; a button that
 * does nothing on this screen gets no box. */
#define BAR_H 34
#define BAR_Y (ST7796_H - BAR_H)
#define BOX_W ((ST7796_W - 12) / 5)

static void button_bar(uint16_t bg) {
    st7796_fill_rect(0, BAR_Y, ST7796_W, BAR_H, bg);
    for (int i = 0; i < 5; i++) {
        const char *label = action_label(BAR[screen][i]);
        if (!label) continue;
        const int scale = label[1] ? 2 : 3;   /* lone - and + read better larger */
        st7796_fill_rect(i * 96, BAR_Y, BOX_W, BAR_H, BTN_COLOR[i]);
        text_centered(i * 96 + BOX_W / 2, BAR_Y + (BAR_H - 7 * scale) / 2, scale,
                      BTN_TEXT[i], BTN_COLOR[i], label);
    }
}

/* Setup screen: one row per setting, each with touchable - and + boxes. */
#define ROW_X   8
#define ROW_W   464
#define ROW_H   74
#define ROW_Y(i) (42 + (i) * 80)
#define PM_W    72
#define PM_H    60
#define MINUS_X 176
#define PLUS_X  392
#define VAL_X   (MINUS_X + PM_W + 8)
#define VAL_W   (PLUS_X - 8 - VAL_X)

static void row_border(int i) {
    const uint16_t c = i == field ? C_WHITE : C_ROW;
    const int y = ROW_Y(i);
    st7796_fill_rect(ROW_X, y, ROW_W, 3, c);
    st7796_fill_rect(ROW_X, y + ROW_H - 3, ROW_W, 3, c);
    st7796_fill_rect(ROW_X, y, 3, ROW_H, c);
    st7796_fill_rect(ROW_X + ROW_W - 3, y, 3, ROW_H, c);
}

static void row_value(int i) {
    char v[8];
    const int y = ROW_Y(i);
    snprintf(v, sizeof v, "%d", setting[i]);
    st7796_fill_rect(VAL_X, y + 7, VAL_W, PM_H, C_ROW);
    text_centered(VAL_X + VAL_W / 2, y + 16, 6, C_WHITE, C_ROW, v);
}

static void pm_box(int x, int y, const char *sign) {
    st7796_fill_rect(x, y + 7, PM_W, PM_H, C_GREY);
    text_centered(x + PM_W / 2, y + 7 + (PM_H - 35) / 2, 5, C_WHITE, C_GREY, sign);
}

static void draw_row(int i) {
    const int y = ROW_Y(i);
    st7796_fill_rect(ROW_X, y, ROW_W, ROW_H, C_ROW);
    row_border(i);
    big_text(24, y + 12, 4, FIELD_COLOR[i], C_ROW, FIELD_NAME[i]);
    big_text(24, y + 50, 2, C_GREY, C_ROW, FIELD_UNIT[i]);
    pm_box(MINUS_X, y, "-");
    pm_box(PLUS_X, y, "+");
    row_value(i);
}

static void draw_setup(void) {
    st7796_fill_screen(C_BLACK);
    big_text(8, 10, 3, C_WHITE, C_BLACK, "EXERCISE TIMER");
    big_text(472 - text_w("OK = START", 2), 14, 2, C_GREY, C_BLACK, "OK = START");
    for (int i = 0; i < F_COUNT; i++) draw_row(i);
    button_bar(C_BLACK);
}

/* Running screen. */
#define SECS_Y     80
#define SECS_SCALE 20
#define PROG_X     16
#define PROG_Y     236
#define PROG_W     448
#define PROG_H     28

static uint16_t phase_bg(void) { return phase == PH_FLEX ? C_FLEX : C_REST; }
static uint16_t phase_track(void) { return phase == PH_FLEX ? C_FLEX_TR : C_REST_TR; }

static uint64_t time_left_us(uint64_t now) {
    if (paused) return paused_left_us;
    return phase_end_us > now ? phase_end_us - now : 0;
}

/* The seconds left, rounded up (10 .. 1), with a small SEC after them, the
 * pair centred. Dimmed while paused. */
static void draw_secs(int secs) {
    char d[8];
    const uint16_t bg = phase_bg();
    snprintf(d, sizeof d, "%d", secs);
    const int len = (int)strlen(d), dw = text_w(d, SECS_SCALE), gap = 12;
    const int x = (ST7796_W - dw - gap - text_w("SEC", 3)) / 2;
    if (len != shown_len) st7796_fill_rect(0, SECS_Y, ST7796_W, 7 * SECS_SCALE, bg);
    big_text(x, SECS_Y, SECS_SCALE, paused ? C_DIM : C_WHITE, bg, d);
    big_text(x + dw + gap, SECS_Y + 7 * SECS_SCALE - 21, 3, C_WHITE, bg, "SEC");
    shown_secs = secs;
    shown_len = len;
}

/* Fills left to right as the phase runs; only the new part is drawn. */
static void draw_progress(uint64_t left) {
    const int px = (int)((phase_len_us - left) * PROG_W / phase_len_us);
    if (shown_bar < 0) {
        st7796_fill_rect(PROG_X, PROG_Y, PROG_W, PROG_H, phase_track());
        shown_bar = 0;
    }
    if (px > shown_bar) {
        st7796_fill_rect(PROG_X + shown_bar, PROG_Y, px - shown_bar, PROG_H, C_WHITE);
        shown_bar = px;
    }
}

static void draw_paused_label(void) {
    st7796_fill_rect(PROG_X, PROG_Y, PROG_W, PROG_H, phase_track());
    text_centered(ST7796_W / 2, PROG_Y + 4, 3, C_WHITE, phase_track(), "PAUSED");
}

static void run_update(uint64_t now) {
    const uint64_t left = time_left_us(now);
    const int secs = (int)((left + 999999u) / 1000000u);
    if (secs != shown_secs) draw_secs(secs);
    if (!paused) draw_progress(left);   /* the PAUSED label sits there instead */
}

static void draw_run(void) {
    char n[8];
    const uint16_t bg = phase_bg();
    st7796_fill_screen(bg);
    big_text(16, 12, 8, C_WHITE, bg, phase == PH_FLEX ? "FLEX" : "REST");
    big_text(464 - text_w("REPS LEFT", 2), 12, 2, C_WHITE, bg, "REPS LEFT");
    snprintf(n, sizeof n, "%d", reps_left);
    big_text(464 - text_w(n, 5), 32, 5, C_WHITE, bg, n);
    shown_secs = shown_len = shown_bar = -1;
    if (paused) draw_paused_label();
    button_bar(bg);
    run_update(time_us_64());
}

static void draw_done(void) {
    char s[24];
    st7796_fill_screen(C_DONE);
    text_centered(ST7796_W / 2, 56, 12, C_WHITE, C_DONE, "DONE");
    snprintf(s, sizeof s, "%d REP%s COMPLETE", setting[F_REPS], setting[F_REPS] == 1 ? "" : "S");
    text_centered(ST7796_W / 2, 176, 3, C_WHITE, C_DONE, s);
    button_bar(C_DONE);
}

/* Also the About screen's restore callback: the whole current screen. */
static void redraw(void) {
    if (screen == SCR_SETUP) draw_setup();
    else if (screen == SCR_RUN) draw_run();
    else draw_done();
}

/* ── workout ───────────────────────────────────────────────────────────── */
static void begin_phase(phase_t p, uint64_t start_us) {
    phase = p;
    phase_len_us = (uint64_t)setting[p == PH_FLEX ? F_FLEX : F_REST] * 1000000u;
    phase_end_us = start_us + phase_len_us;
    DIAG("exercise_timer: %s %d s, %d reps left\n", p == PH_FLEX ? "FLEX" : "REST",
         setting[p == PH_FLEX ? F_FLEX : F_REST], reps_left);
}

/* The current phase is over. The next one starts at `at`, where this one
 * ended rather than when a pass noticed, so the timing does not drift.
 * Returns false when the workout is over. */
static bool next_phase(uint64_t at) {
    if (phase == PH_FLEX) {
        if (--reps_left <= 0) return false;
        if (setting[F_REST] > 0) {
            begin_phase(PH_REST, at);
            return true;
        }
    }
    begin_phase(PH_FLEX, at);
    return true;
}

static void start(void) {
    reps_left = setting[F_REPS];
    paused = false;
    screen = SCR_RUN;
    begin_phase(PH_FLEX, time_us_64());
    draw_run();
}

static void finish(void) {
    DIAG("exercise_timer: done, %d reps\n", setting[F_REPS]);
    reps_left = 0;
    paused = false;
    screen = SCR_DONE;
    draw_done();
}

static void run_tick(void) {
    const uint64_t now = time_us_64();
    if (!paused && now >= phase_end_us) {
        do {
            if (!next_phase(phase_end_us)) {
                finish();
                return;
            }
        } while (now >= phase_end_us);
        draw_run();
        return;
    }
    run_update(now);
}

/* Ends the current phase now; a skipped flex counts as a rep. Stays paused
 * if it was, with the whole next phase left. */
static void skip(void) {
    if (!next_phase(time_us_64())) {
        finish();
        return;
    }
    if (paused) paused_left_us = phase_len_us;
    draw_run();
}

static void toggle_pause(void) {
    const uint64_t now = time_us_64();
    if (paused) {
        phase_end_us = now + paused_left_us;
        paused = false;
    } else {
        paused_left_us = time_left_us(now);
        paused = true;
        draw_paused_label();
    }
    shown_secs = shown_bar = -1;   /* digit colour changes; bar comes back */
    button_bar(phase_bg());
    run_update(now);
}

/* ── setup editing ─────────────────────────────────────────────────────── */
static void select_field(int f) {
    const int old = field;
    field = (f + F_COUNT) % F_COUNT;
    row_border(old);
    row_border(field);
}

/* One step in direction dir (+1 / -1); a coarse step lands on the next
 * multiple of 5. */
static void adjust(int dir, bool coarse) {
    int v = setting[field];
    if (!coarse) v += dir;
    else if (dir > 0) v = (v / 5 + 1) * 5;
    else v = ((v + 4) / 5 - 1) * 5;
    if (v < MIN_V[field]) v = MIN_V[field];
    if (v > MAX_V[field]) v = MAX_V[field];
    if (v == setting[field]) return;
    setting[field] = v;
    row_value(field);
}

static void defaults(void) {
    memcpy(setting, DEFAULTS, sizeof setting);
    for (int i = 0; i < F_COUNT; i++) row_value(i);
}

static void action_run(action_t a) {
    switch (a) {
    case ACT_NEXT_FIELD: select_field(field + 1); break;
    case ACT_PREV_FIELD: select_field(field - 1); break;
    case ACT_DEC:        adjust(-1, false); break;
    case ACT_INC:        adjust(+1, false); break;
    case ACT_START:
    case ACT_AGAIN:      start(); break;
    case ACT_DEFAULTS:   defaults(); break;
    case ACT_SKIP:       skip(); break;
    case ACT_PAUSE:      toggle_pause(); break;
    case ACT_STOP:
    case ACT_SETUP:
        paused = false;
        screen = SCR_SETUP;
        draw_setup();
        break;
    default:             break;
    }
}

/* ── input ─────────────────────────────────────────────────────────────── */

/* A held - / + (button or touch) repeats: first after 400 ms, then every
 * 100 ms, in coarse steps after 2 s. */
static action_t rep_act = ACT_NONE;
static int rep_btn = -1;   /* the key holding it, or -1 for touch */
static uint32_t rep_since, rep_next;

static void repeat_begin(action_t a, int btn, uint32_t now) {
    if (a != ACT_INC && a != ACT_DEC) {
        rep_act = ACT_NONE;
        return;
    }
    rep_act = a;
    rep_btn = btn;
    rep_since = now;
    rep_next = now + 400;
}

static void repeat_task(uint32_t now) {
    if (rep_act == ACT_NONE || screen != SCR_SETUP) return;
    if ((int32_t)(now - rep_next) < 0) return;
    rep_next = now + 100;
    adjust(rep_act == ACT_INC ? +1 : -1, now - rep_since >= 2000);
}

static action_t key_action(uartkbd_btn_t b) {
    if (b <= UARTKBD_BTN_RED) return BAR[screen][b];
    switch (b) {
    case UARTKBD_BTN_NAV_UP:    return screen == SCR_SETUP ? ACT_PREV_FIELD : ACT_NONE;
    case UARTKBD_BTN_NAV_DOWN:  return screen == SCR_SETUP ? ACT_NEXT_FIELD : ACT_NONE;
    case UARTKBD_BTN_NAV_LEFT:  return screen == SCR_SETUP ? ACT_DEC : ACT_NONE;
    case UARTKBD_BTN_NAV_RIGHT: return screen == SCR_SETUP ? ACT_INC : ACT_NONE;
    case UARTKBD_BTN_OK:
    case UARTKBD_BTN_NAV_CENTER:
        return screen == SCR_SETUP ? ACT_START : screen == SCR_RUN ? ACT_PAUSE : ACT_AGAIN;
    case UARTKBD_BTN_CANCEL:
        return screen == SCR_RUN ? ACT_STOP : screen == SCR_DONE ? ACT_SETUP : ACT_NONE;
    default:                    return ACT_NONE;
    }
}

static void keys_poll(uint32_t now) {
    uartkbd_event_t ev;
    while (uartkbd_next_event(&ev)) {
        if (!ev.pressed) {
            if ((int)ev.btn == rep_btn) rep_act = ACT_NONE;
            continue;
        }
        const action_t a = key_action(ev.btn);
        action_run(a);
        repeat_begin(a, (int)ev.btn, now);
    }
}

/* The button bar (same rectangles as the labels, 4 px of slack above) on
 * every screen; on setup, a row selects it and its - / + adjust it. */
static action_t touch_hit(int x, int y, int *row) {
    *row = -1;
    if (y + 4 >= BAR_Y) {
        const int i = x / 96;
        return i < 5 && x - i * 96 < BOX_W ? BAR[screen][i] : ACT_NONE;
    }
    if (screen != SCR_SETUP) return ACT_NONE;
    for (int i = 0; i < F_COUNT; i++) {
        const int ry = ROW_Y(i);
        if (y < ry || y >= ry + ROW_H || x < ROW_X || x >= ROW_X + ROW_W) continue;
        *row = i;
        if (x >= MINUS_X && x < MINUS_X + PM_W) return ACT_DEC;
        if (x >= PLUS_X && x < PLUS_X + PM_W) return ACT_INC;
        return ACT_NONE;
    }
    return ACT_NONE;
}

static void touch_poll(uint32_t now) {
    static bool was_down;
    uint16_t x, y;
    const bool down = ft6336_poll(&x, &y);
    if (down && !was_down) {
        int row;
        const action_t a = touch_hit(x, y, &row);
        if (row >= 0 && row != field) select_field(row);
        action_run(a);
        repeat_begin(a, -1, now);
    } else if (!down && was_down && rep_btn == -1) {
        rep_act = ACT_NONE;
    }
    was_down = down;
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

int main(void) {
    board_init();
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd_restore(redraw);
    agentio_init();   /* before the first clear: it owns the capture shadow */
    memcpy(setting, DEFAULTS, sizeof setting);
    draw_setup();
    board_backlight_set(1);
    const bool touch_ok = ft6336_init();
    picpwr_release_unused();
    DIAG("exercise_timer: up (touch %s)\n", touch_ok ? "ok" : "MISSING");

    uint32_t next_touch = now_ms();
    for (;;) {
        fw2_app_recovery_task();
        picpwr_task();
        agentio_task();
        const uint32_t now = now_ms();
        keys_poll(now);
        if ((int32_t)(now - next_touch) >= 0) {
            next_touch = now + 20;
            touch_poll(now);
        }
        repeat_task(now);
        if (screen == SCR_RUN) run_tick();
        fw2_app_recovery_sleep_ms(5);
    }
}
