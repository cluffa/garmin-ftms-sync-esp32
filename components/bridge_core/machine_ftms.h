#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "machine_adapter.h"
#include "ftms_devlist.h"

// Store the own address type inferred by ble_hs_id_infer_auto() in on_host_sync.
// Must be called before machine_ftms_start_scan().
void machine_ftms_set_addr_type(uint8_t addr_type);

// Store the data callback; called on each parsed Treadmill Data notification.
void machine_ftms_set_data_cb(machine_state_cb cb);

// Report connect(1)/disconnect(0) to the facade (machine.c owns reconnect).
void machine_ftms_set_event_cb(machine_event_cb cb);

// True if the advert carries the FTMS service (0x1826) — used by the facade's
// unified scan to classify a device.
bool machine_ftms_is_ftms_adv(const uint8_t *data, uint8_t len);

// Cancel any active scan, terminate any existing connection, then connect to dev.
void machine_ftms_connect(const ftms_device_t *dev);

// (Used internally / legacy; the facade machine.c owns the unified scan + list.)
void machine_ftms_start_scan(void);
int  machine_ftms_get_devices(ftms_device_t *out, int max);

bool   machine_ftms_connected(void);
bool   machine_ftms_connecting(void);
int8_t machine_ftms_conn_rssi(void);

/* Write FTMS Control Point. Returns false if not connected or CP not found. */
bool machine_ftms_set_speed(float kmh);
bool machine_ftms_set_incline(float pct);
bool machine_ftms_stop(void);
