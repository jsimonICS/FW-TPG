/* Timing constants for a simplified EIA RS-170 / NTSC-derived 15.734 kHz RGB
 * signal: a fixed 262-line, non-interlaced field (every field draws the same
 * lines -- a common "240p"-style simplification of true 262.5/263 interlace)
 * repeated forever, targeting classic 15 kHz arcade RGB monitors.
 *
 * All durations are in nanoseconds and are multiples of 5 ns so that they
 * convert to a whole number of cycles at this BSP's fixed 200 MHz clk_sys
 * (FWOG_SYS_CLK_KHZ, bsp/common/clocks.h) with zero rounding error. The
 * conversion itself is still done from clock_get_hz() at runtime (never a
 * hardcoded divider/cycle count) per AGENTS.md's clock invariant.
 *
 * UNVERIFIED ON HARDWARE. These numbers are derived from published EIA
 * RS-170/NTSC reference values, not measured against a real 15 kHz monitor
 * or a scope. Treat every constant here as a starting point for bring-up,
 * not a proven-correct spec.
 *
 * CSYNC is assumed ACTIVE-LOW (0 = sync asserted, 1 = idle), the common
 * convention for a TTL-level composite-sync line feeding an external
 * resistor/transistor sync-mixing circuit. If your hardware expects the
 * opposite polarity, flip CSYNC_IDLE/CSYNC_ASSERT below -- nothing else
 * changes. */
#ifndef TPG_VIDEO_TIMING_H
#define TPG_VIDEO_TIMING_H
#include <stdint.h>

/* --- Horizontal (one line = 63.56 us, H-rate ~15733.8 Hz) --- */
#define TPG_H_TOTAL_NS        63560u   /* whole line */
#define TPG_HSYNC_NS           4700u   /* sync pulse */
#define TPG_BACK_PORCH_NS       4700u
#define TPG_FRONT_PORCH_NS      1520u  /* 1.5 us nominal, +20 ns so the
                                          active region divides evenly into
                                          8 color bars -- see ACTIVE_NS */
#define TPG_ACTIVE_NS \
    (TPG_H_TOTAL_NS - TPG_HSYNC_NS - TPG_BACK_PORCH_NS - TPG_FRONT_PORCH_NS)
#define TPG_BAR_COUNT 8
#define TPG_BAR_NS            (TPG_ACTIVE_NS / TPG_BAR_COUNT)   /* 6580 ns */

/* --- Crosshatch grid geometry, shared by both crosshatch patterns. A closed
 * grid of TPG_GRID_CELLS x 6 square cells (on a 4:3 screen): 9 vertical
 * lines, the first starting the active region and the last ending it, and 7
 * horizontal ones on active lines 0, 40, ..., 240 -- so both edges and the
 * center carry a line, which is what H/V size, position and linearity get
 * adjusted against. 8 pitches + one line fill TPG_ACTIVE_NS exactly, and
 * every width is a multiple of 20 ns, so it is a whole number of cycles at
 * both 200 MHz (here) and 150 MHz (the FW2 ZoomIO port, gen_tpg_zio.py).
 * The optional dots sit at each cell's center. */
#define TPG_GRID_CELLS           8u
#define TPG_GRID_LINE_NS       320u
#define TPG_GRID_PITCH_NS     6540u   /* one line's start to the next's */
#define TPG_GRID_GAP_NS        (TPG_GRID_PITCH_NS - TPG_GRID_LINE_NS)
#define TPG_GRID_DOT_NS        300u
#define TPG_GRID_DOT_SIDE_NS   ((TPG_GRID_GAP_NS - TPG_GRID_DOT_NS) / 2u)   /* 2960 ns */
#define TPG_GRID_ROW_SPACING    40u
_Static_assert(TPG_GRID_CELLS * TPG_GRID_PITCH_NS + TPG_GRID_LINE_NS == TPG_ACTIVE_NS,
               "crosshatch must span the active region exactly");
_Static_assert(2u * TPG_GRID_DOT_SIDE_NS + TPG_GRID_DOT_NS == TPG_GRID_GAP_NS,
               "dot must be centered in its cell");

/* --- Vertical sync structure (EIA RS-170: 3 pre-eq + 3 vsync + 3 post-eq
 * lines, each carrying two half-line-rate pulses) --- */
#define TPG_HALF_LINE_NS      (TPG_H_TOTAL_NS / 2)     /* 31780 ns */
#define TPG_EQ_PULSE_NS         2300u                  /* equalizing pulse */
#define TPG_EQ_HIGH_NS        (TPG_HALF_LINE_NS - TPG_EQ_PULSE_NS)
#define TPG_VSYNC_SERRATION_NS  4700u                  /* serration notch,
                                                           same width as
                                                           ordinary hsync */
#define TPG_VSYNC_LOW_NS      (TPG_HALF_LINE_NS - TPG_VSYNC_SERRATION_NS)

/* --- Field line layout (262 lines/field, fixed, no true interlace) --- */
#define TPG_LINES_PRE_EQ         3u
#define TPG_LINES_VSYNC          3u
#define TPG_LINES_POST_EQ        3u
#define TPG_LINES_PRE_BLANK     10u   /* ordinary blanked lines before active video */
#define TPG_LINES_ACTIVE       241u   /* 240 + 1 so the crosshatch's bottom edge
                                         line lands a whole cell below the last */
#define TPG_LINES_POST_BLANK     2u   /* ordinary blanked lines after active video */
#define TPG_LINES_PER_FIELD \
    (TPG_LINES_PRE_EQ + TPG_LINES_VSYNC + TPG_LINES_POST_EQ + \
     TPG_LINES_PRE_BLANK + TPG_LINES_ACTIVE + TPG_LINES_POST_BLANK)   /* 262 */

/* Segments per line type -> total words per field. Both bus8_words[] and
 * bus2_words[] (video_pattern.c) are exactly this length: one word per
 * segment, both buses always advancing in lockstep. */
#define TPG_SEGS_PER_EQ_LINE     4u   /* pulse, high, pulse, high */
#define TPG_SEGS_PER_VSYNC_LINE  4u   /* low, serration, low, serration */
#define TPG_SEGS_PER_BLANK_LINE  4u   /* front porch, sync, back porch, blanked active */

/* Active-line segment count varies by pattern (video_pattern.c): bars,
 * checkerboard and the solids/gradients use few segments; a crosshatch line
 * is porches + a line and gap per cell + the closing line, and the worst is
 * crosshatch+dots' dot lines (two per cell row), whose gaps split into
 * gap/dot/gap. TPG_FIELD_WORDS_MAX sizes the shared field buffers for that
 * worst case; tpg_pattern_build() returns the actual word count it wrote for
 * whichever pattern was requested, which is always <= this. */
#define TPG_SEGS_PER_GRID_LINE   (3u + 2u * TPG_GRID_CELLS + 1u)   /* 20 */
#define TPG_SEGS_PER_DOT_LINE    (3u + 4u * TPG_GRID_CELLS + 1u)   /* 36 */
#define TPG_GRID_DOT_LINES       (2u * (TPG_LINES_ACTIVE / TPG_GRID_ROW_SPACING))

#define TPG_FIELD_WORDS_MAX \
    ((TPG_LINES_PRE_EQ + TPG_LINES_POST_EQ) * TPG_SEGS_PER_EQ_LINE + \
     TPG_LINES_VSYNC * TPG_SEGS_PER_VSYNC_LINE + \
     (TPG_LINES_PRE_BLANK + TPG_LINES_POST_BLANK) * TPG_SEGS_PER_BLANK_LINE + \
     TPG_GRID_DOT_LINES * TPG_SEGS_PER_DOT_LINE + \
     (TPG_LINES_ACTIVE - TPG_GRID_DOT_LINES) * TPG_SEGS_PER_GRID_LINE)   /* 5096 */

/* ns -> PIO cycles at the CURRENT clk_sys, rounded to nearest. Call once
 * clocks are up; never hardcode 200 MHz here even though this BSP's
 * clk_sys is fixed at 200 MHz today (AGENTS.md: derive from
 * clock_get_hz(), don't assume a divider/rate). */
static inline uint32_t tpg_ns_to_cycles(uint32_t ns, uint32_t sys_hz) {
    return (uint32_t)(((uint64_t)ns * sys_hz + 500000000ull) / 1000000000ull);
}

#endif
