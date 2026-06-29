#include "rsc_encode.h"

#define RSC_FLAG_TOTAL_DISTANCE (1 << 1)
#define RSC_FLAG_RUNNING        (1 << 2)

static uint32_t round_u32(float v) { return (uint32_t)(v + 0.5f); }

size_t rsc_encode_measurement(const treadmill_state_t *s, uint8_t *out,
                              size_t cap) {
    if (cap < 8) return 0;
    uint16_t speed_256 = (uint16_t)round_u32(s->speed_mps * 256.0f);
    uint32_t dist_dm = round_u32(s->distance_m * 10.0f);

    out[0] = RSC_FLAG_TOTAL_DISTANCE | RSC_FLAG_RUNNING;
    out[1] = speed_256 & 0xFF;
    out[2] = (speed_256 >> 8) & 0xFF;
    out[3] = 0xFF; // instantaneous cadence — 0xFF means invalid, forces Garmin wrist fallback
    out[4] = dist_dm & 0xFF;
    out[5] = (dist_dm >> 8) & 0xFF;
    out[6] = (dist_dm >> 16) & 0xFF;
    out[7] = (dist_dm >> 24) & 0xFF;
    return 8;
}
