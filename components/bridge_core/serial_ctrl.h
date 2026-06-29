#pragma once
#include "model.h"

/* Start the USB-serial command task. Call after machine_set_data_cb(). */
void serial_ctrl_start(void);

/* Push a treadmill state update to any connected serial client. */
void serial_ctrl_push_state(const treadmill_state_t *s);

/* Push a connection event (connected != 0) or disconnect (0). */
void serial_ctrl_push_event(int connected, const char *name, const char *proto);
