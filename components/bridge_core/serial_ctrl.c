/*
 * serial_ctrl.c — USB-serial command interface for the phone relay path.
 *
 * On chips with native USB Serial/JTAG (ESP32-C6, ESP32-S3 native USB):
 *   uses usb_serial_jtag_driver_install() + VFS so fgets/printf work.
 * On chips with external USB-UART (Heltec V3 / CP210x on S3):
 *   uses uart_driver_install(UART_NUM_0) + VFS.
 *
 * Both paths expose stdin/stdout through IDF VFS, so the read task uses
 * fgets() and the output helpers use printf() — no platform ifdefs in logic.
 *
 * Commands (one line each, ASCII):
 *   SCAN               → {"cmd":"scan","ok":true}
 *   LIST               → {"cmd":"list","devices":[...]}
 *   CONNECT <idx>      → {"cmd":"connect","ok":true}
 *   SPEED <kmh>        → {"cmd":"speed","ok":true}
 *   INCLINE <pct>      → {"cmd":"incline","ok":true}
 *   STATUS             → {"cmd":"status",...}
 *
 * Events pushed proactively:
 *   {"event":"state","speed":...,"distance":...,"incline":...,"elapsed":...}
 *   {"event":"connected","name":"...","proto":"..."}
 *   {"event":"disconnected"}
 */

#include "serial_ctrl.h"
#include "machine.h"
#include "ftms_devlist.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* Use USB Serial/JTAG when the console is routed that way (e.g. ESP32-C6 XIAO),
 * otherwise fall back to UART0 (e.g. Heltec V3 with CP210x). */
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#  include "driver/usb_serial_jtag.h"
#  include "driver/usb_serial_jtag_vfs.h"
#else
#  include "driver/uart.h"
#  include "esp_vfs_dev.h"        /* esp_vfs_dev_uart_use_driver() */
#endif

#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG            "serial_ctrl"
#define BUF_SIZE       256
#define STATE_RATE_MS  1000   /* push state events at most once per second */

static treadmill_state_t s_last_state;
static bool              s_state_valid;
static SemaphoreHandle_t s_tx_mutex;  /* serialize printf from multiple tasks */
static int64_t           s_last_state_us;

/* ---- output ------------------------------------------------------------ */

static void tx(const char *line)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    printf("%s\n", line);
    fflush(stdout);
    xSemaphoreGive(s_tx_mutex);
}

/* ---- command handlers -------------------------------------------------- */

static void cmd_scan(void)
{
    machine_start_scan();
    tx("{\"cmd\":\"scan\",\"ok\":true}");
}

static void cmd_list(void)
{
    ftms_device_t devs[FTMS_MAX_DEVICES];
    int n = machine_get_devices(devs, FTMS_MAX_DEVICES);
    char buf[512];
    int pos = snprintf(buf, sizeof buf, "{\"cmd\":\"list\",\"devices\":[");
    for (int i = 0; i < n && pos < (int)sizeof(buf) - 80; i++) {
        const char *proto = devs[i].proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS";
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"idx\":%d,\"name\":\"%s\",\"proto\":\"%s\",\"rssi\":%d}",
                        i ? "," : "", i, devs[i].name, proto, devs[i].rssi);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    tx(buf);
}

static void cmd_connect(int idx)
{
    ftms_device_t devs[FTMS_MAX_DEVICES];
    int n = machine_get_devices(devs, FTMS_MAX_DEVICES);
    if (idx < 0 || idx >= n) {
        tx("{\"cmd\":\"connect\",\"ok\":false,\"err\":\"bad index\"}");
        return;
    }
    machine_connect(&devs[idx]);
    tx("{\"cmd\":\"connect\",\"ok\":true}");
}

static void cmd_speed(float kmh)
{
    bool ok = machine_set_speed(kmh);
    char buf[64];
    snprintf(buf, sizeof buf, "{\"cmd\":\"speed\",\"ok\":%s}", ok ? "true" : "false");
    tx(buf);
}

static void cmd_incline(float pct)
{
    bool ok = machine_set_incline(pct);
    char buf[64];
    snprintf(buf, sizeof buf, "{\"cmd\":\"incline\",\"ok\":%s}", ok ? "true" : "false");
    tx(buf);
}

static void cmd_status(void)
{
    bool conn = machine_connected();
    char buf[256];
    if (!conn || !s_state_valid) {
        snprintf(buf, sizeof buf, "{\"cmd\":\"status\",\"connected\":%s}",
                 conn ? "true" : "false");
    } else {
        const treadmill_state_t *s = &s_last_state;
        snprintf(buf, sizeof buf,
                 "{\"cmd\":\"status\",\"connected\":true"
                 ",\"speed\":%.2f,\"distance\":%.1f"
                 ",\"incline\":%.1f,\"elapsed\":%lu}",
                 s->speed_mps * 3.6f, s->distance_m,
                 s->incline_pct, (unsigned long)s->elapsed_s);
    }
    tx(buf);
}

/* ---- line dispatcher --------------------------------------------------- */

static void dispatch(char *line)
{
    while (*line == ' ') line++;
    /* strip trailing \r and \n (fgets keeps \n) */
    int len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';
    if (len == 0) return;

    if (strcmp(line, "SCAN") == 0)          { cmd_scan(); return; }
    if (strcmp(line, "LIST") == 0)          { cmd_list(); return; }
    if (strcmp(line, "STATUS") == 0)        { cmd_status(); return; }
    if (strcmp(line, "STOP") == 0)          { bool ok = machine_stop(); tx(ok ? "{\"cmd\":\"stop\",\"ok\":true}" : "{\"cmd\":\"stop\",\"ok\":false}"); return; }
    if (strncmp(line, "CONNECT ", 8) == 0)  { cmd_connect(atoi(line + 8)); return; }
    if (strncmp(line, "SPEED ", 6) == 0)    { cmd_speed((float)atof(line + 6)); return; }
    if (strncmp(line, "INCLINE ", 8) == 0)  { cmd_incline((float)atof(line + 8)); return; }
    tx("{\"event\":\"error\",\"msg\":\"unknown command\"}");
}

/* ---- read task --------------------------------------------------------- */

static void serial_task(void *arg)
{
    (void)arg;
    char line[BUF_SIZE];
    while (1) {
        if (fgets(line, sizeof line, stdin) != NULL) {
            dispatch(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* ---- public API -------------------------------------------------------- */

void serial_ctrl_start(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();       /* route stdin/stdout through USB CDC */
    ESP_LOGI(TAG, "serial_ctrl ready on USB Serial/JTAG");
#else
    uart_driver_install(UART_NUM_0, BUF_SIZE * 2, 0, 0, NULL, 0);
    esp_vfs_dev_uart_use_driver(0);         /* route stdin/stdout through UART0 */
    ESP_LOGI(TAG, "serial_ctrl ready on UART0");
#endif

    xTaskCreate(serial_task, "serial_ctrl", 4096, NULL, 5, NULL);
}

void serial_ctrl_push_state(const treadmill_state_t *s)
{
    s_last_state  = *s;
    s_state_valid = true;
    int64_t now = esp_timer_get_time();
    if (now - s_last_state_us < STATE_RATE_MS * 1000LL) return;
    s_last_state_us = now;
    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"event\":\"state\",\"speed\":%.2f,\"distance\":%.1f"
             ",\"incline\":%.1f,\"elapsed\":%lu}",
             s->speed_mps * 3.6f, s->distance_m,
             s->incline_pct, (unsigned long)s->elapsed_s);
    tx(buf);
}

void serial_ctrl_push_event(int connected, const char *name, const char *proto)
{
    char buf[128];
    if (connected) {
        snprintf(buf, sizeof buf,
                 "{\"event\":\"connected\",\"name\":\"%s\",\"proto\":\"%s\"}",
                 name ? name : "", proto ? proto : "");
    } else {
        snprintf(buf, sizeof buf, "{\"event\":\"disconnected\"}");
    }
    tx(buf);
}
