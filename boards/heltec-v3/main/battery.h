#pragma once
#include <stdint.h>

/* LiPo open-circuit voltage → state-of-charge (single cell). Pure; host-tested.
 * Clamps to 0/100 outside the curve. */
uint8_t battery_pct_from_mv(uint16_t mv);

/* State-of-charge % → 0..4 (5-level bar bucket). Pure; host-tested. The flat
 * middle of the LiPo curve makes a precise % read jumpy, so the home screen
 * shows a coarse bucket instead. */
uint8_t battery_bucket(uint8_t pct);

/* Initialize the battery ADC (Heltec V3: divider on GPIO1, control on GPIO37). */
void battery_init(void);

/* Battery voltage in millivolts (averaged over 8 ADC samples). */
uint16_t battery_read_mv(void);
