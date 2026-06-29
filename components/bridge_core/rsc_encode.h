#pragma once
#include <stddef.h>
#include <stdint.h>
#include "model.h"

size_t rsc_encode_measurement(const treadmill_state_t *s, uint8_t *out,
                              size_t cap);
