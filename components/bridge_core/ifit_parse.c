#include "ifit_parse.h"

bool ifit_parse_data(const uint8_t *b, size_t len,
                     float *speed_mps, float *incline_pct) {
    if (len != 20) return false;
    if (b[0] != 0x00 || b[1] != 0x12 || b[2] != 0x01 || b[3] != 0x04)
        return false;   /* not a speed/incline data frame */

    uint16_t spd = (uint16_t)b[10] | ((uint16_t)b[11] << 8);          /* 0.01 km/h */
    int16_t  inc = (int16_t)((uint16_t)b[12] | ((uint16_t)b[13] << 8)); /* 0.01 %  */

    *speed_mps   = (spd / 100.0f) * (1000.0f / 3600.0f);
    *incline_pct = inc / 100.0f;
    return true;
}
