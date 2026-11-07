/* The set of test patterns, shared by both CPUs: main (video_pattern.c)
 * generates them on the breakout header, display (display/main.c) names
 * them on the LCD and cycles through them on GREEN/YELLOW. One enum and one
 * name table for both, so the two can't silently drift out of step. */
#ifndef TPG_PATTERNS_H
#define TPG_PATTERNS_H

/* Order is the button-cycle order, video_pattern.c's dispatch table order,
 * and the wire order sent in tpg_set_pattern_t.pattern_id (tpg_link_proto.h)
 * -- keep all three in step with this. */
typedef enum {
    TPG_PATTERN_COLOR_BARS = 0,
    TPG_PATTERN_RGB_GRADIENT,
    TPG_PATTERN_BW_GRADIENT,
    TPG_PATTERN_CHECKERBOARD,
    TPG_PATTERN_CROSSHATCH_DOTS,
    TPG_PATTERN_RGB_CROSSHATCH,
    TPG_PATTERN_SOLID_RED,
    TPG_PATTERN_SOLID_BLUE,
    TPG_PATTERN_SOLID_GREEN,
    TPG_PATTERN_COUNT
} tpg_pattern_id_t;

/* Short display name for `id`, e.g. for the OG's LCD. Never NULL: anything
 * outside 0..TPG_PATTERN_COUNT-1 returns "?". */
const char *tpg_pattern_name(tpg_pattern_id_t id);

#endif
