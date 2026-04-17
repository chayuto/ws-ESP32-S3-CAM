#pragma once

#include "esp_timer.h"

#define CRY_TIMER_START(var) int64_t var = esp_timer_get_time()
#define CRY_TIMER_ELAPSED_US(var) ((int64_t)(esp_timer_get_time() - (var)))
#define CRY_TIMER_ELAPSED_MS(var) ((int32_t)(CRY_TIMER_ELAPSED_US(var) / 1000))
