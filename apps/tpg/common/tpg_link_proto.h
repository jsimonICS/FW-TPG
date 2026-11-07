/* TPG's own application-level link message, riding the same inter-CPU UART
 * link as wiliOGbsp/bsp/common/link/io_proto.h (see link_frame.h: byte 0 of
 * every payload is the message type, 0x00 is invalid, 0x01-0x1F is the
 * bootloader's, 0x20+ is free). io_proto.h already claims 0x20-0x22, so this
 * file starts at 0x30 to leave it (and any future shared BSP protocol)
 * headroom without a collision.
 *
 * Unlike io_proto.h's CONFIG_ENABLE/SET_DIRS, this message carries no `seq`
 * and gets no ack: it is a fire-and-forget UI event (a button press), not a
 * request whose completion main needs to confirm. It carries the display's
 * new pattern index as an absolute value rather than a relative "cycle"
 * step, so a single dropped frame cannot leave the two CPUs' idea of the
 * current pattern permanently out of sync -- the next press still names the
 * right target.
 *
 * Struct goes on the wire as-is, packed, exactly like io_proto.h's -- see
 * that file's header comment for why casting a receive buffer pointer
 * directly into this struct is safe here specifically. */
#ifndef TPG_LINK_PROTO_H
#define TPG_LINK_PROTO_H
#include <stddef.h>
#include <stdint.h>

#define FWOG_TPG_MSG_SET_PATTERN 0x30u   /* display -> main */

typedef struct __attribute__((packed)) {
    uint8_t type;        /* FWOG_TPG_MSG_SET_PATTERN */
    uint8_t pattern_id;  /* tpg_pattern_id_t, video_pattern.h */
} tpg_set_pattern_t;
_Static_assert(sizeof(tpg_set_pattern_t) == 2,
               "tpg_set_pattern_t goes on the wire unserialized");

/* Returns bytes written to out, or 0 if `cap` is too small. */
size_t tpg_link_proto_build_set_pattern(void *out, size_t cap,
                                        uint8_t pattern_id);

/* The message type if `payload` is one of ours and long enough for its own
 * struct, else 0 (io_proto.h reserves 0 as invalid). */
uint8_t tpg_link_proto_type(const void *payload, size_t len);

#endif
