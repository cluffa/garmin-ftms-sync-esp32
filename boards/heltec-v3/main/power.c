#include "power.h"
#include "display.h"
#include "esp_sleep.h"
#include "esp_log.h"

#define PIN_BTN 0   /* Heltec V3 PRG button, active low */

void power_off(void) {
    ESP_LOGI("power", "powering off — press PRG to wake");
    display_off();
    /* Wake when GPIO0 goes low (button pressed). GPIO0 is an RTC GPIO on the
     * ESP32-S3, so ext1 any-low wake works across deep sleep. */
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();   /* does not return; wake = full reboot */
}
