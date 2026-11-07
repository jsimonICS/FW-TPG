#include "video_pattern.h"
#include "pico/assert.h"
#include <stddef.h>

typedef struct {
    uint32_t *bus8;
    uint32_t *bus2;
    uint32_t idx;
    uint32_t sys_hz;
} tpg_builder_t;

/* A 0..7 channel level in its three pins' ascending-GPIO order. Every
 * channel is wired MSB first: its lowest GPIO carries bit 2, the next bit 1
 * and the highest bit 0 (see video_pattern.h's pin table). */
static inline uint32_t wire3(uint8_t v) {
    return (uint32_t)(((v >> 2) & 1u) | (v & 2u) | ((v & 1u) << 2));
}

/* bus8 pattern bits, see video_pattern.h's pin table: B2 B1 B0 G2 G1 G0 R2 R1 */
static inline uint32_t pack_bus8(uint8_t r, uint8_t g, uint8_t b) {
    return wire3(b) | (wire3(g) << 3) | ((wire3(r) & 3u) << 6);
}

/* bus2 pattern bits: bit0 = R0, bit1 = CSYNC (1 = idle/high, 0 = asserted) */
static inline uint32_t pack_bus2(uint8_t r, bool sync_idle) {
    return (wire3(r) >> 2) | ((uint32_t)(sync_idle ? 1u : 0u) << 1);
}

/* Appends one (pattern, hold) word to both streams. hold_ns must be >= 15 ns
 * (3 cycles at 200 MHz) -- see tpg_video.pio's word-format comment for why
 * "-3"; every real segment here is at least hundreds of ns, far above that
 * floor. */
static void emit(tpg_builder_t *bd, uint8_t r, uint8_t g, uint8_t b,
                  bool sync_idle, uint32_t hold_ns) {
    hard_assert(bd->idx < TPG_FIELD_WORDS_MAX);
    uint32_t hold_cycles = tpg_ns_to_cycles(hold_ns, bd->sys_hz);
    hard_assert(hold_cycles >= 3u);
    uint32_t rem = hold_cycles - 3u;
    bd->bus8[bd->idx] = pack_bus8(r, g, b) | (rem << TPG_BUS8_PIN_COUNT);
    bd->bus2[bd->idx] = pack_bus2(r, sync_idle) | (rem << TPG_BUS2_PIN_COUNT);
    bd->idx++;
}

/* Pre/post-equalizing line: two 2.3 us pulses at half-line spacing, blanked. */
static void emit_eq_line(tpg_builder_t *bd) {
    emit(bd, 0, 0, 0, false, TPG_EQ_PULSE_NS);
    emit(bd, 0, 0, 0, true, TPG_EQ_HIGH_NS);
    emit(bd, 0, 0, 0, false, TPG_EQ_PULSE_NS);
    emit(bd, 0, 0, 0, true, TPG_EQ_HIGH_NS);
}

/* Vertical-sync line: two serrated broad pulses at half-line spacing. */
static void emit_vsync_line(tpg_builder_t *bd) {
    emit(bd, 0, 0, 0, false, TPG_VSYNC_LOW_NS);
    emit(bd, 0, 0, 0, true, TPG_VSYNC_SERRATION_NS);
    emit(bd, 0, 0, 0, false, TPG_VSYNC_LOW_NS);
    emit(bd, 0, 0, 0, true, TPG_VSYNC_SERRATION_NS);
}

/* Ordinary line, active video blanked to black -- used before/after the
 * active picture so it has a real top/bottom border. */
static void emit_blank_line(tpg_builder_t *bd) {
    emit(bd, 0, 0, 0, true, TPG_FRONT_PORCH_NS);
    emit(bd, 0, 0, 0, false, TPG_HSYNC_NS);
    emit(bd, 0, 0, 0, true, TPG_BACK_PORCH_NS);
    emit(bd, 0, 0, 0, true, TPG_ACTIVE_NS);
}

/* Every active line starts with the same front-porch/hsync/back-porch
 * segments; only what fills TPG_ACTIVE_NS after that differs by pattern. */
static void emit_hsync_porches(tpg_builder_t *bd) {
    emit(bd, 0, 0, 0, true, TPG_FRONT_PORCH_NS);
    emit(bd, 0, 0, 0, false, TPG_HSYNC_NS);
    emit(bd, 0, 0, 0, true, TPG_BACK_PORCH_NS);
}

/* One active-line "fill": emits exactly TPG_ACTIVE_NS worth of segments.
 * `line_idx` is 0..TPG_LINES_ACTIVE-1, for patterns that vary top-to-bottom. */
typedef void (*active_fill_fn)(tpg_builder_t *bd, unsigned line_idx);

/* --- Color bars: classic 8 vertical bars, 100% amplitude, full 3-bit scale --- */
static const uint8_t BAR_R[TPG_BAR_COUNT] = {7, 7, 0, 0, 7, 7, 0, 0};
static const uint8_t BAR_G[TPG_BAR_COUNT] = {7, 7, 7, 7, 0, 0, 0, 0};
static const uint8_t BAR_B[TPG_BAR_COUNT] = {7, 0, 7, 0, 7, 0, 7, 0};

static void fill_bars(tpg_builder_t *bd, unsigned line_idx) {
    (void)line_idx;
    for (unsigned i = 0; i < TPG_BAR_COUNT; i++)
        emit(bd, BAR_R[i], BAR_G[i], BAR_B[i], true, TPG_BAR_NS);
}

/* --- Solid fields --- */
static void fill_solid(tpg_builder_t *bd, uint8_t r, uint8_t g, uint8_t b) {
    emit(bd, r, g, b, true, TPG_ACTIVE_NS);
}
static void fill_solid_red(tpg_builder_t *bd, unsigned line_idx) {
    (void)line_idx; fill_solid(bd, 7, 0, 0);
}
static void fill_solid_blue(tpg_builder_t *bd, unsigned line_idx) {
    (void)line_idx; fill_solid(bd, 0, 0, 7);
}
static void fill_solid_green(tpg_builder_t *bd, unsigned line_idx) {
    (void)line_idx; fill_solid(bd, 0, 7, 0);
}

/* Gradient step 0..7, dividing TPG_LINES_ACTIVE into eight bands of
 * 241 / 8 = 30 lines; the clamp folds the one leftover line into the bottom
 * band. The step is the channel level, so each of a channel's three ladder
 * bits shows up on its own (1, 2, 4) and the ladder's linearity is visible. */
static uint8_t grad_step(unsigned line_idx) {
    unsigned step = line_idx / (TPG_LINES_ACTIVE / 8u);
    return (uint8_t)(step > 7u ? 7u : step);
}

/* --- B&W gradient: 8 horizontal bands, black (0) at the top to white (7) --- */
static void fill_bw_gradient(tpg_builder_t *bd, unsigned line_idx) {
    uint8_t lvl = grad_step(line_idx);
    fill_solid(bd, lvl, lvl, lvl);
}

/* --- RGB gradient: R/G/B columns, each stepping 0..7 top to bottom --- */
static void fill_rgb_gradient(tpg_builder_t *bd, unsigned line_idx) {
    uint8_t lvl = grad_step(line_idx);
    /* TPG_ACTIVE_NS isn't evenly divisible by 3; the last column absorbs the
     * remainder so the three segments still sum to exactly TPG_ACTIVE_NS. */
    uint32_t col_ns = TPG_ACTIVE_NS / 3u;
    uint32_t last_col_ns = TPG_ACTIVE_NS - 2u * col_ns;
    emit(bd, lvl, 0, 0, true, col_ns);
    emit(bd, 0, lvl, 0, true, col_ns);
    emit(bd, 0, 0, lvl, true, last_col_ns);
}

/* --- Checkerboard: TPG_BAR_COUNT columns x 6 rows, alternating black/white,
 * checkered by flipping which columns start white each row band. --- */
static void fill_checkerboard(tpg_builder_t *bd, unsigned line_idx) {
    unsigned row = line_idx / (TPG_LINES_ACTIVE / 6u);
    if (row > 5u) row = 5u;   /* 241 / 6 = 40; see grad_step's note */
    for (unsigned col = 0; col < TPG_BAR_COUNT; col++) {
        uint8_t lvl = ((row + col) % 2u) == 0u ? 7u : 0u;
        emit(bd, lvl, lvl, lvl, true, TPG_BAR_NS);
    }
}

/* --- Crosshatch, plain and RGB: the closed grid in video_timing.h --
 * TPG_GRID_CELLS + 1 vertical lines on every active line, a full-width
 * horizontal line every TPG_GRID_ROW_SPACING lines. Crosshatch+dots adds a
 * 2-line-tall white dot at the center of every cell; the RGB crosshatch
 * cycles its vertical lines through red, green, blue, white instead. */
static bool is_grid_row(unsigned line_idx) {
    return (line_idx % TPG_GRID_ROW_SPACING) == 0u;
}
static bool is_dot_row(unsigned line_idx) {
    unsigned r = line_idx % TPG_GRID_ROW_SPACING;
    return r == TPG_GRID_ROW_SPACING / 2u - 1u || r == TPG_GRID_ROW_SPACING / 2u;
}
static void fill_crosshatch(tpg_builder_t *bd, unsigned line_idx,
                            const uint8_t (*colors)[3], unsigned n_colors,
                            bool dots) {
    if (is_grid_row(line_idx)) {
        emit(bd, 7, 7, 7, true, TPG_ACTIVE_NS);
        return;
    }
    bool dot_row = dots && is_dot_row(line_idx);
    for (unsigned col = 0; col < TPG_GRID_CELLS; col++) {
        const uint8_t *c = colors[col % n_colors];
        emit(bd, c[0], c[1], c[2], true, TPG_GRID_LINE_NS);
        if (dot_row) {
            emit(bd, 0, 0, 0, true, TPG_GRID_DOT_SIDE_NS);
            emit(bd, 7, 7, 7, true, TPG_GRID_DOT_NS);
            emit(bd, 0, 0, 0, true, TPG_GRID_DOT_SIDE_NS);
        } else {
            emit(bd, 0, 0, 0, true, TPG_GRID_GAP_NS);
        }
    }
    const uint8_t *c = colors[TPG_GRID_CELLS % n_colors];
    emit(bd, c[0], c[1], c[2], true, TPG_GRID_LINE_NS);
}
static void fill_crosshatch_dots(tpg_builder_t *bd, unsigned line_idx) {
    static const uint8_t WHITE[1][3] = {{7, 7, 7}};
    fill_crosshatch(bd, line_idx, WHITE, 1u, true);
}
static void fill_rgb_crosshatch(tpg_builder_t *bd, unsigned line_idx) {
    static const uint8_t RGBW[4][3] = {{7, 0, 0}, {0, 7, 0}, {0, 0, 7}, {7, 7, 7}};
    fill_crosshatch(bd, line_idx, RGBW, 4u, false);
}

/* Order matches tpg_pattern_id_t (common/tpg_patterns.h), the single source
 * of truth shared with the display's name table (common/tpg_patterns.c). */
static const active_fill_fn PATTERN_FILL[TPG_PATTERN_COUNT] = {
    [TPG_PATTERN_COLOR_BARS]      = fill_bars,
    [TPG_PATTERN_RGB_GRADIENT]    = fill_rgb_gradient,
    [TPG_PATTERN_BW_GRADIENT]     = fill_bw_gradient,
    [TPG_PATTERN_CHECKERBOARD]    = fill_checkerboard,
    [TPG_PATTERN_CROSSHATCH_DOTS] = fill_crosshatch_dots,
    [TPG_PATTERN_RGB_CROSSHATCH]  = fill_rgb_crosshatch,
    [TPG_PATTERN_SOLID_RED]       = fill_solid_red,
    [TPG_PATTERN_SOLID_BLUE]      = fill_solid_blue,
    [TPG_PATTERN_SOLID_GREEN]     = fill_solid_green,
};

uint32_t tpg_pattern_build(uint32_t *bus8, uint32_t *bus2, uint32_t sys_hz,
                           tpg_pattern_id_t pattern) {
    hard_assert(pattern < TPG_PATTERN_COUNT);
    tpg_builder_t bd = {.bus8 = bus8, .bus2 = bus2, .idx = 0, .sys_hz = sys_hz};
    active_fill_fn fill = PATTERN_FILL[pattern];

    for (unsigned i = 0; i < TPG_LINES_PRE_EQ; i++) emit_eq_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_VSYNC; i++) emit_vsync_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_POST_EQ; i++) emit_eq_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_PRE_BLANK; i++) emit_blank_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_ACTIVE; i++) {
        emit_hsync_porches(&bd);
        fill(&bd, i);
    }
    for (unsigned i = 0; i < TPG_LINES_POST_BLANK; i++) emit_blank_line(&bd);

    hard_assert(bd.idx <= TPG_FIELD_WORDS_MAX);
    return bd.idx;
}
