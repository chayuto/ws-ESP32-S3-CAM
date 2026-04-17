#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t noise_floor_init(uint32_t warmup_s, float margin);

/* Submit a raw-sample RMS value. Called once per audio chunk (e.g., 160 ms). */
void noise_floor_submit_rms(float rms);

/* Once warmup has elapsed, returns a threshold *delta* to add to the base
 * detection threshold. Before warmup is complete, returns 0. Value range
 * [0 .. margin]. */
float noise_floor_threshold_adjust(void);

/* Read-only accessors for the web UI. */
float noise_floor_p50(void);
float noise_floor_p95(void);
bool noise_floor_is_warm(void);
uint32_t noise_floor_remaining_warmup_s(void);
