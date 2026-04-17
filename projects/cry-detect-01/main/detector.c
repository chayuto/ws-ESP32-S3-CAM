#include "detector.h"

#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "detector";

static float s_threshold;
static uint32_t s_consec_needed;
static uint32_t s_hold_us;
static detector_state_cb_t s_cb;
static void *s_ctx;

static uint32_t s_streak;
static detector_state_t s_state;
static int64_t s_alert_until_us;

void detector_init(float threshold_conf, uint32_t consec_frames, uint32_t hold_ms,
                   detector_state_cb_t cb, void *ctx)
{
    s_threshold = threshold_conf;
    s_consec_needed = consec_frames;
    s_hold_us = (uint32_t)hold_ms * 1000u;
    s_cb = cb;
    s_ctx = ctx;
    s_streak = 0;
    s_state = DETECTOR_IDLE;
    s_alert_until_us = 0;
    ESP_LOGI(TAG, "threshold=%.2f consec=%u hold_ms=%u", (double)s_threshold,
             (unsigned)consec_frames, (unsigned)hold_ms);
}

void detector_set_threshold(float threshold_conf)
{
    s_threshold = threshold_conf;
}

float detector_get_threshold(void)
{
    return s_threshold;
}

void detector_submit(float cry_conf)
{
    int64_t now = esp_timer_get_time();

    if (cry_conf >= s_threshold) {
        s_streak++;
    } else {
        s_streak = 0;
    }

    if (s_state == DETECTOR_IDLE && s_streak >= s_consec_needed) {
        s_state = DETECTOR_CRYING;
        s_alert_until_us = now + s_hold_us;
        if (s_cb) s_cb(s_state, cry_conf, s_ctx);
    } else if (s_state == DETECTOR_CRYING) {
        if (cry_conf >= s_threshold) {
            s_alert_until_us = now + s_hold_us;
        } else if (now >= s_alert_until_us) {
            s_state = DETECTOR_IDLE;
            s_streak = 0;
            if (s_cb) s_cb(s_state, cry_conf, s_ctx);
        }
    }
}

detector_state_t detector_get_state(void)
{
    return s_state;
}
