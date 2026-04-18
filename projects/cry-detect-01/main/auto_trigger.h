#pragma once

#include "esp_err.h"

/* RMS-threshold auto-trigger: fires event_recorder_trigger_manual() when
 * input_rms sustains above (multiplier * noise_floor_p95) for sustain_ms.
 * Model-independent fallback for data collection while the INT8 model is
 * saturated. Cooldown prevents re-firing until cooldown_s has elapsed. */
esp_err_t auto_trigger_init(void);
