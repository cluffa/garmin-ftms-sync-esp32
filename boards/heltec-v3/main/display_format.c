#include "display_format.h"
#include "battery.h"
#include <stdio.h>
#include <string.h>

void device_label(const ftms_device_t *d, char *out, int n) {
    if (d->name[0] != '\0') {
        snprintf(out, n, "%s", d->name);
    } else {
        snprintf(out, n, "%s-%02X%02X",
                 d->proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS",
                 d->addr[4], d->addr[5]);
    }
}

void display_format_home(const display_model_t *m, char lines[DISP_LINES][DISP_COLS]) {
    char bar[5];   /* 4-cell battery bar, '#' filled / '.' empty */
    uint8_t b = battery_bucket(battery_pct_from_mv(m->battery_mv));
    for (int i = 0; i < 4; i++) bar[i] = (i < b) ? '#' : '.';
    bar[4] = '\0';
    int mv = m->battery_mv > 9999 ? 9999 : m->battery_mv;  /* bound volt digits */
    snprintf(lines[0], DISP_COLS, "%-9.9s %d.%02dV %s",
             m->device_name, mv/1000, (mv%1000)/10, bar);
    if (m->mill_connected)
        snprintf(lines[1], DISP_COLS, "Tread: CONN %ddBm", m->rssi);
    else if (m->mill_connecting)
        snprintf(lines[1], DISP_COLS, "Tread: CONNECTING");
    else
        snprintf(lines[1], DISP_COLS, "Tread: SCANNING");
    const char *w = m->watch_subscribed ? "SUBSCRIBED"
                  : m->advertising      ? "ADV" : "CONNECTED";
    snprintf(lines[2], DISP_COLS, "Watch: %s", w);
    if (m->imperial)
        snprintf(lines[3], DISP_COLS, " %.1f mph  %.2f mi",
                 m->state.speed_mps * 2.236936f, m->state.distance_m / 1609.344f);
    else
        snprintf(lines[3], DISP_COLS, " %.1f km/h  %.2f km",
                 m->state.speed_mps * 3.6f, m->state.distance_m / 1000.0f);
    snprintf(lines[4], DISP_COLS, " incline %+.1f%%", m->state.incline_pct);
}

void display_format_menu(const ftms_device_t *devs, int n, int sel,
                         char lines[DISP_LINES][DISP_COLS]) {
    snprintf(lines[0], DISP_COLS, "Select treadmill:");
    for (int r = 0; r < 3; r++) lines[1+r][0] = '\0';
    if (n <= 0) {
        snprintf(lines[1], DISP_COLS, " No FTMS devices");
    } else {
        /* 3-row window that keeps sel visible */
        int top = 0;
        if (n > 3) { top = sel - 1; if (top < 0) top = 0; if (top > n - 3) top = n - 3; }
        for (int r = 0; r < 3 && top + r < n; r++) {
            int i = top + r;
            char mark = (i == sel) ? '>' : ' ';
            char label[FTMS_NAME_LEN];
            device_label(&devs[i], label, sizeof label);
            /* name truncated to leave room for rssi */
            snprintf(lines[1+r], DISP_COLS, "%c%-12.12s %d", mark, label, devs[i].rssi);
        }
    }
    snprintf(lines[4], DISP_COLS, "short=next long=ok");
}
