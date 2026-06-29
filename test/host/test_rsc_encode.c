#include <assert.h>
#include <stdio.h>
#include "rsc_encode.h"

int main(void) {
    treadmill_state_t s = {0};
    s.speed_mps = 8.0f * 1000.0f / 3600.0f; // 2.2222 m/s
    s.distance_m = 1234.5f;
    uint8_t out[8];
    size_t n = rsc_encode_measurement(&s, out, sizeof out);
    assert(n == 8);
    assert(out[0] == 0x06);              // flags: total distance + running
    // speed = round(2.2222 * 256) = 569 = 0x0239
    assert(out[1] == 0x39 && out[2] == 0x02);
    assert(out[3] == 0xFF);              // cadence: invalid (0xFF)= 0
    // distance = round(1234.5 * 10) = 12345 = 0x00003039
    assert(out[4] == 0x39 && out[5] == 0x30 && out[6] == 0x00 && out[7] == 0x00);
    printf("rsc_encode: OK\n");
    return 0;
}
