/* Builds the one-field-long word streams tpg_video.pio's two state machines
 * DMA from. See tpg_video.pio for the word format and video_timing.h for
 * the timing this walks. */
#ifndef TPG_VIDEO_PATTERN_H
#define TPG_VIDEO_PATTERN_H
#include <stdint.h>
#include "video_timing.h"
#include "common/tpg_patterns.h"

/* Pin layout (see docs/hardware/pinmap.md and gpio/breakout.h for why these
 * ten and not GPIO 16/17):
 *   bus8, GPIO 8..15, bit0..bit7 = B2 B1 B0 G2 G1 G0 R2 R1
 *   bus2, GPIO 26..27, bit0..bit1 = R0 CSYNC (CSYNC active-low)
 * MSB first within each channel, blue to red: the order the external R/G/B
 * resistor ladders are wired in. Each channel's bit 2 has the 120 ohm
 * resistor, bit 1 240 ohm, bit 0 470 ohm (a 4:2:1 ladder).
 *   blue:  B2 GPIO 8 (hdr 9),   B1 GPIO 9 (hdr 5),   B0 GPIO 10 (hdr 7)
 *   green: G2 GPIO 11 (hdr 11), G1 GPIO 12 (hdr 12), G0 GPIO 13 (hdr 1)
 *   red:   R2 GPIO 14 (hdr 15), R1 GPIO 15 (hdr 13), R0 GPIO 26 (hdr 14) */
#define TPG_BUS8_BASE_PIN 8u
#define TPG_BUS8_PIN_COUNT 8u
#define TPG_BUS2_BASE_PIN 26u
#define TPG_BUS2_PIN_COUNT 2u

/* Fill bus8[TPG_FIELD_WORDS_MAX] and bus2[TPG_FIELD_WORDS_MAX] with one field
 * of `pattern` at the timing in video_timing.h, and return the number of
 * words actually written (<= TPG_FIELD_WORDS_MAX -- different patterns use
 * different active-line segment counts, see video_timing.h). sys_hz is the
 * live clk_sys (clock_get_hz(clk_sys)) used to convert every ns constant to
 * a cycle count -- see video_timing.h's tpg_ns_to_cycles(). */
uint32_t tpg_pattern_build(uint32_t *bus8, uint32_t *bus2, uint32_t sys_hz,
                           tpg_pattern_id_t pattern);

#endif
