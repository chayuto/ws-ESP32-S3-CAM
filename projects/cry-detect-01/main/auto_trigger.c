#include "auto_trigger.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

#include "metrics.h"
#include "event_recorder.h"
#include "noise_floor.h"

static const char *TAG = "autotrig";

#define POLL_MS    100

static void auto_trigger_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    const float    multiplier  = (float)CONFIG_CRY_AUTO_TRIG_MULTIPLIER;
    const float    abs_floor   = (float)CONFIG_CRY_AUTO_TRIG_ABS_RMS;
    const uint32_t sustain_ms  = CONFIG_CRY_AUTO_TRIG_SUSTAIN_MS;
    const uint32_t cooldown_us = (uint32_t)CONFIG_CRY_AUTO_TRIG_COOLDOWN_S * 1000000u;

    /* Use p50 (median ambient) as the baseline, not p95. p95 already
     * captures the noisiest 5% of ambient — triggering above that window
     * misses real events buried in typical room noise. p50 is a robust
     * "typical quiet" reference. */
    ESP_LOGI(TAG, "auto-trigger: %.1fx nf_p50, floor=%.0f, sustain=%ums, cooldown=%us",
             (double)multiplier, (double)abs_floor,
             (unsigned)sustain_ms, (unsigned)CONFIG_CRY_AUTO_TRIG_COOLDOWN_S);

    int64_t above_since_us = 0;
    int64_t last_fire_us   = 0;
    uint32_t fire_count    = 0;

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        int64_t now_us = esp_timer_get_time();

        /* Skip while noise floor is still warming up — firing before we
         * have a baseline would mean comparing against 0 → constant trigger. */
        if (!noise_floor_is_warm()) {
            above_since_us = 0;
            continue;
        }

        /* Cooldown: don't re-fire within the window, regardless of signal. */
        if (last_fire_us > 0 && (uint32_t)(now_us - last_fire_us) < cooldown_us) {
            above_since_us = 0;
            continue;
        }

        cry_metrics_t m;
        metrics_snapshot(&m);
        float rms   = m.input_rms;
        float nf50  = noise_floor_p50();
        float thr   = multiplier * nf50;
        if (thr < abs_floor) thr = abs_floor;

        if (rms > thr) {
            if (above_since_us == 0) above_since_us = now_us;
            uint32_t dur_ms = (uint32_t)((now_us - above_since_us) / 1000);
            if (dur_ms >= sustain_ms) {
                /* Label row records what we crossed by — useful for
                 * culling in the morning (rare big events vs. sustained
                 * mid-level events vs. false positives). */
                char note[48];
                snprintf(note, sizeof(note), "auto-rms-%.0fx-rms%.0f",
                         (double)(rms / (nf50 > 1.0f ? nf50 : 1.0f)),
                         (double)rms);
                bool ok = event_recorder_trigger_manual(note);
                if (ok) {
                    fire_count++;
                    last_fire_us = now_us;
                    ESP_LOGI(TAG, "FIRE #%u: rms=%.0f nf50=%.0f thr=%.0f (%s)",
                             (unsigned)fire_count, (double)rms, (double)nf50,
                             (double)thr, note);
                } else {
                    /* Recorder already busy — reset the sustain so next
                     * crossing has to sustain again from scratch. */
                    ESP_LOGD(TAG, "skipped: recorder busy");
                }
                above_since_us = 0;
            }
        } else {
            above_since_us = 0;
        }
    }
}

esp_err_t auto_trigger_init(void)
{
#if !CONFIG_CRY_AUTO_TRIG_ENABLED
    ESP_LOGI(TAG, "auto-trigger disabled by Kconfig");
    return ESP_OK;
#else
    BaseType_t ok = xTaskCreatePinnedToCore(
        auto_trigger_task, "autotrig", 4 * 1024, NULL, 3, NULL, tskNO_AFFINITY);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
#endif
}
