#pragma once
#include <stdbool.h>
#include "model.h"
#include "ftms_devlist.h"

#define DISP_LINES 5
#define DISP_COLS  21

typedef struct {
    bool mill_connected;
    bool mill_connecting;        /* a connect/reconnect attempt is in flight */
    bool watch_subscribed;
    bool advertising;
    bool imperial;               /* show mph/mi instead of km-h/km */
    uint16_t battery_mv;         /* battery millivolts */
    char device_name[FTMS_NAME_LEN]; /* connected device label, "" if none */
    int8_t rssi;                 /* connected treadmill RSSI (dBm); 0 if n/a */
    treadmill_state_t state;
} display_model_t;

void display_format_home(const display_model_t *m, char lines[DISP_LINES][DISP_COLS]);
void display_format_menu(const ftms_device_t *devs, int n, int sel,
                         char lines[DISP_LINES][DISP_COLS]);
void device_label(const ftms_device_t *d, char *out, int n);
