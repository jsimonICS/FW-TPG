#include "video_pattern.h"
#include "pico/assert.h"

typedef struct {
    uint32_t *bus8;
    uint32_t *bus2;
    uint32_t idx;
    uint32_t sys_hz;
} tpg_builder_t;

/* bus8 pattern bits, see video_pattern.h's pin table: R2 R1 R0 G2 G1 G0 B2 B1 */
static inline uint32_t pack_bus8(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)((r >> 2) & 1u) << 0) | ((uint32_t)((r >> 1) & 1u) << 1) |
           ((uint32_t)(r & 1u) << 2) |
           ((uint32_t)((g >> 2) & 1u) << 3) | ((uint32_t)((g >> 1) & 1u) << 4) |
           ((uint32_t)(g & 1u) << 5) |
           ((uint32_t)((b >> 2) & 1u) << 6) | ((uint32_t)((b >> 1) & 1u) << 7);
}

/* bus2 pattern bits: bit0 = B0, bit1 = CSYNC (1 = idle/high, 0 = asserted) */
static inline uint32_t pack_bus2(uint8_t b, bool sync_idle) {
    return (uint32_t)(b & 1u) | ((uint32_t)(sync_idle ? 1u : 0u) << 1);
}

/* Appends one (pattern, hold) word to both streams. hold_ns must be >= 15 ns
 * (3 cycles at 200 MHz) -- see tpg_video.pio's word-format comment for why
 * "-3"; every real segment here is microseconds, far above that floor. */
static void emit(tpg_builder_t *bd, uint8_t r, uint8_t g, uint8_t b,
                  bool sync_idle, uint32_t hold_ns) {
    hard_assert(bd->idx < TPG_FIELD_WORDS);
    uint32_t hold_cycles = tpg_ns_to_cycles(hold_ns, bd->sys_hz);
    hard_assert(hold_cycles >= 3u);
    uint32_t rem = hold_cycles - 3u;
    bd->bus8[bd->idx] = pack_bus8(r, g, b) | (rem << TPG_BUS8_PIN_COUNT);
    bd->bus2[bd->idx] = pack_bus2(b, sync_idle) | (rem << TPG_BUS2_PIN_COUNT);
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
 * color-bar rows so the picture has a real top/bottom border. */
static void emit_blank_line(tpg_builder_t *bd) {
    emit(bd, 0, 0, 0, true, TPG_FRONT_PORCH_NS);
    emit(bd, 0, 0, 0, false, TPG_HSYNC_NS);
    emit(bd, 0, 0, 0, true, TPG_BACK_PORCH_NS);
    emit(bd, 0, 0, 0, true, TPG_ACTIVE_NS);
}

/* Classic 8 vertical color bars, 100% amplitude, full 3-bit scale. */
static const uint8_t BAR_R[TPG_BAR_COUNT] = {7, 7, 0, 0, 7, 7, 0, 0};
static const uint8_t BAR_G[TPG_BAR_COUNT] = {7, 7, 7, 7, 0, 0, 0, 0};
static const uint8_t BAR_B[TPG_BAR_COUNT] = {7, 0, 7, 0, 7, 0, 7, 0};

static void emit_active_line(tpg_builder_t *bd) {
    emit(bd, 0, 0, 0, true, TPG_FRONT_PORCH_NS);
    emit(bd, 0, 0, 0, false, TPG_HSYNC_NS);
    emit(bd, 0, 0, 0, true, TPG_BACK_PORCH_NS);
    for (unsigned i = 0; i < TPG_BAR_COUNT; i++) {
        emit(bd, BAR_R[i], BAR_G[i], BAR_B[i], true, TPG_BAR_NS);
    }
}

void tpg_pattern_build(uint32_t *bus8, uint32_t *bus2, uint32_t sys_hz) {
    tpg_builder_t bd = {.bus8 = bus8, .bus2 = bus2, .idx = 0, .sys_hz = sys_hz};

    for (unsigned i = 0; i < TPG_LINES_PRE_EQ; i++) emit_eq_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_VSYNC; i++) emit_vsync_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_POST_EQ; i++) emit_eq_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_PRE_BLANK; i++) emit_blank_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_ACTIVE; i++) emit_active_line(&bd);
    for (unsigned i = 0; i < TPG_LINES_POST_BLANK; i++) emit_blank_line(&bd);

    hard_assert(bd.idx == TPG_FIELD_WORDS);
}
