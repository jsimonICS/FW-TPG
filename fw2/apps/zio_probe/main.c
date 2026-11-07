/* zio_probe — can a display RAM app start a ZoomIO program on the main CPU?
 *
 * Feasibility probe for the ZoomIO TPG. ZoomIO runs on MAIN's RISC-V core1
 * and is compiled and launched by the stock MAIN firmware (`s\b\r <path>`),
 * so a display app should be able to drive it over the FwGUI link with
 * neither CPU's flash touched. This app:
 *
 *   1. selects the 3.3 V header VIO rail (the header is silently dead without
 *      it) and holds power zone 6 (FPGA), which run_zio requires and which has
 *      no POWER_ZONES name;
 *   2. has the stock firmware drive header GPIO 25 low, so the pin is set up
 *      as a plain SIO output before ZoomIO's setiodir/setio touch it;
 *   3. writes a 250 ms square-wave program to /appdata/zio_probe/blink.zio,
 *      asks MAIN to run it, and shows MAIN's reply text (compile errors,
 *      EPOWERZONE) on the LCD;
 *   4. samples GPIO 25 back over OneWili and reports the edge count and the
 *      measured half-period (also each edge on RTT). 250 ms means ZoomIO's
 *      cycle model matches MAIN's real execution speed; anything else means
 *      tpg.zio's cycle counts need rescaling. YELLOW steps through three
 *      equivalent programs that localize a mismatch (see programs[]).
 *
 * MAIN gates header GPIO and ZoomIO on power zone 6 using zone reports the
 * stock DISPLAY firmware normally sends, so this app sends them itself.
 *
 * The readback is MAIN's internal pin state (invariant 11): it proves core1
 * is toggling, not that the header pin moves. Confirm that on a scope or LED.
 *
 * GREEN re-runs the program, RED stops it. HOLD HOME 5 S to leave: that
 * resets only the display CPU, so press RED first or core1 keeps toggling. */
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define APP_DIR    "/appdata/zio_probe"
#define ZIO_PATH   APP_DIR "/blink.zio"
#define PROBE_PIN  25
#define HALF_MS    250

/* Three square waves, each nominally 250 ms per level at ZoomIO's assumed
 * 150 MHz, so the measured half-period says where any timing error lives:
 *   MS   — real-time unit, converted to cycles by MAIN's compiler;
 *   CYC  — the same delay as an explicit cycle count (37.5 M cycles);
 *   LOOP — 2500 x 15000-cycle waits, the short-wait-in-a-loop shape the TPG
 *          is built from (loop overhead adds ~0.05%). */
#define BLINK(delay)                                   \
    "setiodir 25, 1\n"                                 \
    "name cnt = a0\n"                                  \
    "while 1\n"                                        \
    "    setio 25, 1\n" delay                          \
    "    setio 25, 0\n" delay                          \
    "endwhile\n"

static const struct { const char *name; const char *zio; } programs[] = {
    { "MS",   BLINK("    wait 250 ms\n") },
    { "CYC",  BLINK("    wait 37500000 cycles\n") },
    { "LOOP", BLINK("    let cnt = 0\n"
                    "    while cnt < 2500\n"
                    "        wait 15000 cycles\n"
                    "        let cnt++\n"
                    "    endwhile\n") },
};
#define N_PROGRAMS (int)(sizeof programs / sizeof programs[0])
static int program;

/* ~37 KB of link buffers — far too big for the 2 KB stack. */
static ow_device dev;

static inline uint16_t be16(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}

#define C_BLACK be16(0x0000)
#define C_WHITE be16(0xFFFF)
#define C_GREEN be16(0x07E0)
#define C_RED   be16(0xF800)
#define C_GREY  be16(0x8410)

/* Status rows at scale 2 (16 px glyphs, 29 columns inside the margins). */
enum { ROW_LINK, ROW_POWER, ROW_PROGRAM, ROW_STEP, ROW_REPLY, ROW_REPLY2, ROW_PIN,
       ROW_EDGES, ROW_PERIOD, ROW_COUNT };
#define ROW_Y(r) (34 + (r) * 26)

static void line(int row, uint16_t fg, const char *fmt, ...) {
    char s[30];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    st7796_fill_rect(8, ROW_Y(row), 464, 16, C_BLACK);
    st7796_draw_text(8, ROW_Y(row), 2, fg, C_BLACK, s);
}

/* Bottom-row labels, one fifth per button (wilibsp AGENTS.md), grey and blue
 * slots left empty because those buttons do nothing here. */
static void button_bar(void) {
    const int w = (ST7796_W - 12) / 5, h = 34, y = ST7796_H - h;
    st7796_fill_rect(1 * 96, y, w, h, 0x06FF);   /* yellow, wire order */
    st7796_draw_text(1 * 96 + 14, y + 9, 2, C_BLACK, 0x06FF, "NEXT");
    st7796_fill_rect(2 * 96, y, w, h, 0x0012);   /* green, wire order */
    st7796_draw_text(2 * 96 + 22, y + 9, 2, C_WHITE, 0x0012, "RUN");
    st7796_fill_rect(4 * 96, y, w, h, 0x0780);   /* red, wire order */
    st7796_draw_text(4 * 96 + 14, y + 9, 2, C_WHITE, 0x0780, "STOP");
}

/* Edge statistics for the readback of PROBE_PIN. */
static int last_level = -1;
static uint32_t edges, read_errors;
static uint64_t first_edge_us, last_edge_us;

static void stats_reset(void) {
    last_level = -1;
    edges = read_errors = 0;
    first_edge_us = last_edge_us = 0;
}

static void sample(void) {
    uint32_t bits;
    if (ow_io_gpio_read_all(&dev, &bits) != OW_OK) {
        read_errors++;
        return;
    }
    const int level = (int)((bits >> PROBE_PIN) & 1u);
    const uint64_t now = time_us_64();
    if (last_level >= 0 && level != last_level) {
        DIAG("zio_probe: %s edge %u -> %d, %u ms since last\n", programs[program].name,
             (unsigned)edges, level, edges ? (unsigned)((now - last_edge_us) / 1000u) : 0u);
        if (edges == 0) first_edge_us = now;
        last_edge_us = now;
        edges++;
    }
    last_level = level;
}

static void show_stats(void) {
    line(ROW_PIN, C_WHITE, "GPIO %d NOW %s", PROBE_PIN,
         last_level < 0 ? "?" : last_level ? "HIGH" : "LOW");
    line(ROW_EDGES, C_WHITE, "EDGES %lu  READ ERR %lu",
         (unsigned long)edges, (unsigned long)read_errors);
    if (edges >= 3) {
        /* Tenths of a ms; error shrinks as edges accumulate (one poll ~ few ms). */
        const uint64_t tenths = (last_edge_us - first_edge_us) / (edges - 1) / 100u;
        line(ROW_PERIOD, C_WHITE, "HALF %lu.%lu MS (WANT %d)",
             (unsigned long)(tenths / 10), (unsigned long)(tenths % 10), HALF_MS);
    } else {
        line(ROW_PERIOD, C_GREY, "HALF -- (WANT %d MS)", HALF_MS);
    }
}

/* Show MAIN's reply body across two rows (it is free text on failure). */
static void show_reply(const char *reply, bool ok) {
    char a[30] = "", b[30] = "";
    snprintf(a, sizeof a, "%s", reply[0] ? reply : (ok ? "(OK, NO TEXT)" : "(ERR, NO TEXT)"));
    if (strlen(reply) > 29) snprintf(b, sizeof b, "%s", reply + 29);
    line(ROW_REPLY, ok ? C_GREEN : C_RED, "%s", a);
    line(ROW_REPLY2, ok ? C_GREEN : C_RED, "%s", b);
}

/* Send one menu command through the raw path so MAIN's reply text survives:
 * the generated bindings report only OK / FAILED, and a failure's text
 * (compile errors, EPOWERZONE, ...) is what this probe exists to see. */
static bool call(const char *label, const char *cmd) {
    static char reply[OW_RESP_MAX];
    int ok = 0;
    line(ROW_STEP, C_WHITE, "%s...", label);
    ow_raw_stash_clear();
    if (ow_raw_send(&dev, cmd) < 0) {
        DIAG("zio_probe: %s send failed\n", label);
        show_reply("SEND FAILED", false);
        line(ROW_STEP, C_RED, "%s FAILED", label);
        return false;
    }
    ow_status s = ow_raw_next_response(&dev, reply, sizeof reply, &ok, 10000);
    if (s != OW_OK) {
        DIAG("zio_probe: %s no response, status %d\n", label, (int)s);
        show_reply(s == OW_ERR_TIMEOUT ? "NO REPLY (TIMEOUT)" : "NO REPLY (LINK ERROR)", false);
        line(ROW_STEP, C_RED, "%s FAILED", label);
        return false;
    }
    DIAG("zio_probe: %s ok=%d reply '%s'\n", label, ok, reply);
    show_reply(reply, ok != 0);
    line(ROW_STEP, ok ? C_GREEN : C_RED, "%s %s", label, ok ? "OK" : "FAILED");
    return ok != 0;
}

/* MAIN gates header GPIO and ZoomIO on zone 6 using the zone state DISPLAY
 * reports (event 48). The stock DISPLAY firmware sends that; a RAM app that
 * replaced it must too, or MAIN keeps refusing with EPOWERZONE 6 FPGA. */
static void report_zones(void) {
    uint32_t rails;
    if (picpwr_rails(&rails)) ow_fwgui_send_power_zones(rails);
}

static void start(void) {
    char cmd[160];
    stats_reset();
    report_zones();
    fw2_app_recovery_sleep_ms(100);   /* fire-and-forget: let MAIN apply it */
    (void)call("STOP_ZIO", "s\\b\\s");   /* fails harmlessly if nothing runs */

    snprintf(cmd, sizeof cmd, "i\\g\\l %d", PROBE_PIN);
    if (!call("GPIO LOW", cmd)) return;

    line(ROW_PROGRAM, C_WHITE, "PROGRAM %s", programs[program].name);
    line(ROW_STEP, C_WHITE, "WRITING BLINK.ZIO...");
    (void)ow_sd_mkdir(&dev, "/appdata");   /* exists after the first run */
    (void)ow_sd_mkdir(&dev, APP_DIR);
    const char *zio = programs[program].zio;
    ow_status s = ow_sd_put_mem(&dev, ZIO_PATH, zio, strlen(zio), false);
    if (s != OW_OK) {
        DIAG("zio_probe: put_mem status %d (sdfs %d)\n", (int)s, (int)ow_sd_last_error());
        line(ROW_STEP, C_RED, "SD WRITE FAILED (%d)", (int)s);
        return;
    }

    snprintf(cmd, sizeof cmd, "s\\b\\r %s", ZIO_PATH);
    (void)call("RUN_ZIO", cmd);
}

static void stop(void) {
    (void)call("STOP_ZIO", "s\\b\\s");
}

/* Hold the FPGA rail and wait (bounded) until the coprocessor reports it. */
static bool fpga_zone_up(void) {
    const uint32_t bit = picpwr_zone_bit(PICPWR_ZONE_FPGA);
    picpwr_keep_awake(bit);
    for (uint32_t waited = 0; waited < 5000; waited += 10) {
        uint32_t rails = 0;
        if (picpwr_rails(&rails) && (rails & bit)) return true;
        fw2_app_recovery_sleep_ms(10);
    }
    return false;
}

int main(void) {
    board_init();   /* must precede ow_open_fwgui: uart_init reads clk_peri */
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    agentio_init();   /* before the first clear: it owns the capture shadow */
    st7796_fill_screen(C_BLACK);
    st7796_draw_text(8, 8, 2, C_WHITE, C_BLACK, "ZOOMIO LAUNCH PROBE");
    button_bar();
    board_backlight_set(1);
    ioexp_vref(VREF_3V3);   /* required: no VIO rail => the header pin cannot drive */

    line(ROW_POWER, C_WHITE, "FPGA ZONE 6...");
    const bool fpga = fpga_zone_up();
    picpwr_release_unused();
    DIAG("zio_probe: FPGA zone %s\n", fpga ? "up" : "NOT reported up");
    line(ROW_POWER, fpga ? C_GREEN : C_RED, "FPGA ZONE 6 %s", fpga ? "ON" : "NOT ON");

    line(ROW_LINK, C_WHITE, "CONNECTING TO MAIN...");
    while (fw2_app_recovery_open_onewili(&dev) != OW_OK) {
        fw2_app_recovery_task();
        DIAG("zio_probe: FwGUI link open failed (is the main CPU running stock fw?), retry in 1 s\n");
        line(ROW_LINK, C_RED, "MAIN LINK FAILED, RETRY");
        fw2_app_recovery_sleep_ms(1000);
    }
    if (fw2_app_recovery_wrap_sd() != OW_OK) {
        DIAG("zio_probe: recovery-aware SD transport setup failed\n");
        line(ROW_LINK, C_RED, "SD TRANSPORT SETUP FAILED");
    } else {
        line(ROW_LINK, C_GREEN, "MAIN LINK UP");
    }

    start();

    uint64_t next_draw_us = 0, next_report_us = 0;
    for (;;) {
        fw2_app_recovery_task();
        agentio_task();

        uartkbd_event_t ev;
        while (uartkbd_next_event(&ev)) {
            if (!ev.pressed) continue;
            if (ev.btn == UARTKBD_BTN_GREEN) start();
            else if (ev.btn == UARTKBD_BTN_RED) stop();
            else if (ev.btn == UARTKBD_BTN_YELLOW) {
                program = (program + 1) % N_PROGRAMS;
                start();
            }
        }

        sample();
        if (time_us_64() >= next_draw_us) {
            show_stats();
            next_draw_us = time_us_64() + 250000u;
        }
        if (time_us_64() >= next_report_us) {
            report_zones();   /* MAIN may have rebooted: keep it informed */
            next_report_us = time_us_64() + 1000000u;
        }
        fw2_app_recovery_sleep_ms(10);
    }
}
