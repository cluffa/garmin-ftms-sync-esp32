#include "display.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"
#include <string.h>

#define TAG          "display"
#define PIN_SDA      17
#define PIN_SCL      18
#define PIN_RST      21
#define PIN_VEXT     36          /* active low: drive LOW to power the OLED */
#define OLED_ADDR    0x3C

static u8g2_t s_u8g2;
static bool   s_ok = false;
static i2c_master_dev_handle_t s_dev;

/* u8g2 HAL: collect bytes during a transfer, flush on END. */
static uint8_t s_txbuf[32];
static uint8_t s_txlen;

static uint8_t u8x8_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
    case U8X8_MSG_BYTE_INIT:       return 1;
    case U8X8_MSG_BYTE_START_TRANSFER: s_txlen = 0; return 1;
    case U8X8_MSG_BYTE_SEND: {
        uint8_t *d = (uint8_t *)arg_ptr;
        while (arg_int-- && s_txlen < sizeof s_txbuf) s_txbuf[s_txlen++] = *d++;
        return 1;
    }
    case U8X8_MSG_BYTE_END_TRANSFER:
        return i2c_master_transmit(s_dev, s_txbuf, s_txlen, 100) == ESP_OK;
    default: return 1;
    }
}

static uint8_t u8x8_gpio_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
    switch (msg) {
    case U8X8_MSG_DELAY_MILLI: vTaskDelay(pdMS_TO_TICKS(arg_int)); return 1;
    default: return 1;
    }
}

void display_init(void) {
    /* Power the OLED via Vext (active low) and pulse reset. */
    gpio_set_direction(PIN_VEXT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_VEXT, 0);
    gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed — display disabled"); return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "OLED not found — display disabled"); return;
    }

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R0,
                                           u8x8_byte_cb, u8x8_gpio_delay_cb);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    s_ok = true;
    ESP_LOGI(TAG, "OLED ready");
}

void display_draw(const char lines[DISP_LINES][DISP_COLS], int bar_pct) {
    if (!s_ok) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tf);
    for (int i = 0; i < DISP_LINES; i++) {
        u8g2_DrawStr(&s_u8g2, 0, 10 + i * 12, lines[i]);
    }
    if (bar_pct >= 0) {                       /* long-press loading bar */
        if (bar_pct > 100) bar_pct = 100;
        u8g2_DrawFrame(&s_u8g2, 0, 60, 128, 4);
        int w = (126 * bar_pct) / 100;
        if (w > 0) u8g2_DrawBox(&s_u8g2, 1, 61, w, 2);
    }
    u8g2_SendBuffer(&s_u8g2);
}

void display_off(void) {
    if (s_ok) u8g2_SetPowerSave(&s_u8g2, 1);
    gpio_set_level(PIN_VEXT, 1);   /* Vext active-low: high = OLED unpowered */
}
