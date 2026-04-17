#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DETECTOR_IDLE,
    DETECTOR_CRYING,
} detector_state_t;

typedef void (*detector_state_cb_t)(detector_state_t new_state,
                                    float trigger_conf,
                                    void *ctx);

void detector_init(float threshold_conf,
                   uint32_t consec_frames,
                   uint32_t hold_ms,
                   detector_state_cb_t cb,
                   void *ctx);

void detector_set_threshold(float threshold_conf);
float detector_get_threshold(void);

void detector_submit(float cry_conf);

detector_state_t detector_get_state(void);
