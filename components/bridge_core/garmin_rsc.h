#pragma once
#include "model.h"
#include <stdbool.h>
#include <stdint.h>

// Set the own address type inferred by ble_hs_id_infer_auto() in on_host_sync.
// Must be called before garmin_rsc_start().
void garmin_rsc_set_addr_type(uint8_t addr_type);

// Register the RSC GATT service + device name. MUST be called from the host
// task before nimble_port_run() (i.e. before the host starts), otherwise the
// service is not in the attribute table and the central can't see it.
void garmin_rsc_register_gatt(void);

void garmin_rsc_start(void);
void garmin_rsc_update(const treadmill_state_t *s);

// Push battery state-of-charge (0..100 %) to the standard Battery Service
// (0x180F). Dedups internally; notifies the watch only when the value changes.
void garmin_rsc_update_battery(uint8_t pct);

// True once a central has enabled notifications on RSC Measurement.
bool garmin_rsc_subscribed(void);
// True while connectable advertising is active.
bool garmin_rsc_advertising(void);
