#include "tpg_patterns.h"

/* Order matches tpg_pattern_id_t exactly. */
static const char *const PATTERN_NAME[TPG_PATTERN_COUNT] = {
    [TPG_PATTERN_COLOR_BARS]      = "COLOR BARS",
    [TPG_PATTERN_RGB_GRADIENT]    = "RGB GRADIENT",
    [TPG_PATTERN_BW_GRADIENT]     = "B&W GRADIENT",
    [TPG_PATTERN_CHECKERBOARD]    = "CHECKERBOARD",
    [TPG_PATTERN_CROSSHATCH_DOTS] = "CROSSHATCH+DOTS",
    [TPG_PATTERN_RGB_CROSSHATCH]  = "RGB CROSSHATCH",
    [TPG_PATTERN_SOLID_RED]       = "SOLID RED",
    [TPG_PATTERN_SOLID_BLUE]      = "SOLID BLUE",
    [TPG_PATTERN_SOLID_GREEN]     = "SOLID GREEN",
};

const char *tpg_pattern_name(tpg_pattern_id_t id) {
    return ((unsigned)id < TPG_PATTERN_COUNT) ? PATTERN_NAME[id] : "?";
}
