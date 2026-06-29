#include "battery.h"

/* Single-cell LiPo discharge curve (open-circuit-ish), mV → %. Descending. */
uint8_t battery_pct_from_mv(uint16_t mv) {
    static const struct { uint16_t mv; uint8_t pct; } curve[] = {
        {4200,100},{4150,95},{4110,90},{4080,85},{4020,80},{3980,75},
        {3950,70},{3910,65},{3870,60},{3850,55},{3840,50},{3820,45},
        {3800,40},{3790,35},{3770,30},{3750,25},{3730,20},{3710,15},
        {3690,10},{3610,5},{3270,0},
    };
    const int n = (int)(sizeof curve / sizeof curve[0]);
    if (mv >= curve[0].mv)     return 100;
    if (mv <= curve[n-1].mv)   return 0;
    for (int i = 0; i < n - 1; i++) {
        if (mv <= curve[i].mv && mv > curve[i+1].mv) {
            int dmv = curve[i].mv  - curve[i+1].mv;
            int dp  = curve[i].pct - curve[i+1].pct;
            return (uint8_t)(curve[i+1].pct + (int)(mv - curve[i+1].mv) * dp / dmv);
        }
    }
    return 0;
}

/* 5-level bucket (0..4 filled bars). Coarse on purpose: hides the few-percent
 * noise the flat LiPo curve produces so the bar reads steady, not broken. */
uint8_t battery_bucket(uint8_t pct) {
    if (pct >= 80) return 4;
    if (pct >= 60) return 3;
    if (pct >= 40) return 2;
    if (pct >= 20) return 1;
    return 0;
}

#ifdef ESP_PLATFORM
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ADC_CTRL_PIN  37            /* Heltec V3: drive HIGH to enable VBAT divider */
#define VBAT_CHANNEL  ADC_CHANNEL_0 /* GPIO1 on ESP32-S3 */
#define VBAT_DIVIDER  4.9f          /* (390k+100k)/100k — trim via BATTERY_CAL_MV */

#ifndef CONFIG_BATTERY_CAL_MV
#define CONFIG_BATTERY_CAL_MV 0
#endif

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;
static bool                       s_have_cali;

void battery_init(void) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << ADC_CTRL_PIN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(ADC_CTRL_PIN, 0);   /* divider off (active high) until we read */

    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&u, &s_adc);
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, VBAT_CHANNEL, &c);

    adc_cali_curve_fitting_config_t cc = {
        .unit_id = ADC_UNIT_1, .chan = VBAT_CHANNEL,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_have_cali = (adc_cali_create_scheme_curve_fitting(&cc, &s_cali) == ESP_OK);
}

uint16_t battery_read_mv(void) {
    gpio_set_level(ADC_CTRL_PIN, 1);   /* enable divider (Heltec V3: active high) */
    vTaskDelay(pdMS_TO_TICKS(10));
    int acc = 0, raw = 0;
    for (int i = 0; i < 8; i++) {
        adc_oneshot_read(s_adc, VBAT_CHANNEL, &raw);
        acc += raw;
    }
    gpio_set_level(ADC_CTRL_PIN, 0);   /* disable divider (saves power) */
    raw = acc / 8;

    int mv_pin = raw;
    if (s_have_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv_pin);
    else             mv_pin = raw * 3300 / 4095;   /* rough fallback */

    int mv = (int)((float)mv_pin * VBAT_DIVIDER) + CONFIG_BATTERY_CAL_MV;
    return mv < 0 ? 0 : (uint16_t)mv;
}

#endif /* ESP_PLATFORM */
