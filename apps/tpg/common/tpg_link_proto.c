#include "tpg_link_proto.h"
#include <string.h>

size_t tpg_link_proto_build_set_pattern(void *out, size_t cap,
                                        uint8_t pattern_id) {
    if (cap < sizeof(tpg_set_pattern_t)) return 0u;
    tpg_set_pattern_t m;
    m.type       = FWOG_TPG_MSG_SET_PATTERN;
    m.pattern_id = pattern_id;
    memcpy(out, &m, sizeof m);
    return sizeof m;
}

uint8_t tpg_link_proto_type(const void *payload, size_t len) {
    if (len == 0u) return 0u;
    uint8_t t = ((const uint8_t *)payload)[0];
    switch (t) {
    case FWOG_TPG_MSG_SET_PATTERN:
        return len >= sizeof(tpg_set_pattern_t) ? t : 0u;
    default:
        return 0u;
    }
}
