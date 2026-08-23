/* Builds the one-field-long word streams tpg_video.pio's two state machines
 * DMA from. See tpg_video.pio for the word format and video_timing.h for
 * the timing this walks. */
#ifndef TPG_VIDEO_PATTERN_H
#define TPG_VIDEO_PATTERN_H
#include <stdint.h>
#include "video_timing.h"

/* Pin layout (see docs/hardware/pinmap.md and gpio/breakout.h for why these
 * ten and not GPIO 16/17):
 *   bus8, GPIO 8..15, bit0..bit7 = R2 R1 R0 G2 G1 G0 B2 B1
 *   bus2, GPIO 26..27, bit0..bit1 = B0 CSYNC (CSYNC active-low)
 * The particular MSB-to-LSB pin ordering is arbitrary but fixed; wire the
 * external R/G/B resistor ladders to match it. */
#define TPG_BUS8_BASE_PIN 8u
#define TPG_BUS8_PIN_COUNT 8u
#define TPG_BUS2_BASE_PIN 26u
#define TPG_BUS2_PIN_COUNT 2u

/* Fill bus8[TPG_FIELD_WORDS] and bus2[TPG_FIELD_WORDS] with one field of
 * color-bar test pattern at the timing in video_timing.h. sys_hz is the
 * live clk_sys (clock_get_hz(clk_sys)) used to convert every ns constant to
 * a cycle count -- see video_timing.h's tpg_ns_to_cycles(). */
void tpg_pattern_build(uint32_t *bus8, uint32_t *bus2, uint32_t sys_hz);

#endif
