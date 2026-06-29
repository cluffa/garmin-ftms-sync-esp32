#pragma once

/* Turn the device "off": blank the OLED and enter deep sleep. The PRG button
 * (GPIO0) is configured as the wake source, so a press powers it back on
 * (which reboots and reconnects to the last device). Does not return. */
void power_off(void);
