#include "button.h"

button_event_t button_classify(uint32_t press_ms) {
    if (press_ms >= BTN_VERYLONG_MS) return BTN_VERYLONG;
    if (press_ms >= BTN_LONG_MS)     return BTN_LONG;
    if (press_ms >= BTN_SHORT_MS)    return BTN_SHORT;
    return BTN_NONE;
}

uint8_t button_bar_pct(uint32_t held_ms) {
    /* Three long-press windows: fill 0->100, hold at 100, reverse 100->0.
     * Empty bar (at/after VERYLONG) = power-off armed. */
    if (held_ms <= BTN_LONG_MS)                 /* fill */
        return (uint8_t)((held_ms * 100u) / BTN_LONG_MS);
    if (held_ms <= 2u * BTN_LONG_MS)            /* hold at 100% */
        return 100;
    if (held_ms >= BTN_VERYLONG_MS)             /* armed */
        return 0;
    return (uint8_t)(100u - (held_ms - 2u * BTN_LONG_MS) * 100u / BTN_LONG_MS);
}

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define PIN_BTN 0   /* Heltec V3 PRG button, active low */

static void (*s_cb)(button_event_t);
static void (*s_press_cb)(void);   /* fired on the press-down edge */

/* Live press state, written by button_task and read by button_held_ms()
 * (render task). Single-word/aligned reads; a slightly stale value just makes
 * the progress bar lag one frame. */
static volatile bool    s_down;
static volatile int64_t s_down_us;

bool button_is_down(void) { return s_down; }

uint32_t button_held_ms(void) {
    if (!s_down) return 0;
    int64_t d = esp_timer_get_time() - s_down_us;
    return d > 0 ? (uint32_t)(d / 1000) : 0;
}

static void button_task(void *arg) {
    (void)arg;
    for (;;) {
        bool down = gpio_get_level(PIN_BTN) == 0;   /* active low */
        int64_t now = esp_timer_get_time();
        if (down && !s_down) {
            s_down_us = now; s_down = true;
            if (s_press_cb) s_press_cb();   /* wake the UI so the bar shows now */
        }
        else if (!down && s_down) {
            uint32_t ms = (uint32_t)((now - s_down_us) / 1000);
            s_down = false;
            button_event_t e = button_classify(ms);
            if (e != BTN_NONE && s_cb) s_cb(e);
        }
        vTaskDelay(pdMS_TO_TICKS(10));   /* poll + debounce */
    }
}

void button_init(void (*cb)(button_event_t)) {
    s_cb = cb;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
}

void button_set_press_cb(void (*cb)(void)) {
    s_press_cb = cb;
}
#endif /* ESP_PLATFORM */
