#include "noise_floor.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "nfloor";

/* Simple coarse histogram over log2(rms) bins. Good enough for p50/p95
 * with <1 KB state — we don't need precision here, we need trend. */
#define BINS 32
#define BIN_MIN 5.0f        /* ~32 LSB RMS — below this we clamp to bin 0 */
#define BIN_MAX 14.5f       /* ~23 000 RMS — above this clamps to top bin */

static uint32_t s_hist[BINS];
static uint32_t s_hist_total;
static int64_t s_start_us;
static uint32_t s_warmup_s;
static float s_margin;
static SemaphoreHandle_t s_lock;

static int bin_for(float rms)
{
    if (rms <= 1.0f) return 0;
    float lg = log2f(rms);
    if (lg < BIN_MIN) return 0;
    if (lg >= BIN_MAX) return BINS - 1;
    float span = BIN_MAX - BIN_MIN;
    return (int)((lg - BIN_MIN) * BINS / span);
}

static float bin_to_rms(int b)
{
    float span = BIN_MAX - BIN_MIN;
    return powf(2.0f, BIN_MIN + span * ((float)b + 0.5f) / BINS);
}

static float percentile_locked(float p)
{
    uint32_t target = (uint32_t)((float)s_hist_total * p);
    uint32_t acc = 0;
    for (int i = 0; i < BINS; ++i) {
        acc += s_hist[i];
        if (acc >= target) return bin_to_rms(i);
    }
    return bin_to_rms(BINS - 1);
}

esp_err_t noise_floor_init(uint32_t warmup_s, float margin)
{
    memset(s_hist, 0, sizeof(s_hist));
    s_hist_total = 0;
    s_start_us = esp_timer_get_time();
    s_warmup_s = warmup_s;
    s_margin = margin;
    s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "warmup=%u s, margin=%.2f", (unsigned)warmup_s, (double)margin);
    return ESP_OK;
}

void noise_floor_submit_rms(float rms)
{
    if (!s_lock) return;
    int b = bin_for(rms);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hist[b]++;
    s_hist_total++;
    if (s_hist_total > 65535) {
        for (int i = 0; i < BINS; ++i) s_hist[i] >>= 1;
        s_hist_total >>= 1;
    }
    xSemaphoreGive(s_lock);
}

bool noise_floor_is_warm(void)
{
    if (!s_lock) return false;
    return (esp_timer_get_time() - s_start_us) >= (int64_t)s_warmup_s * 1000000;
}

uint32_t noise_floor_remaining_warmup_s(void)
{
    if (!s_lock) return 0;
    int64_t rem_us = (int64_t)s_warmup_s * 1000000 - (esp_timer_get_time() - s_start_us);
    if (rem_us <= 0) return 0;
    return (uint32_t)(rem_us / 1000000);
}

float noise_floor_p50(void)
{
    if (!s_lock) return 0.0f;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    float v = percentile_locked(0.50f);
    xSemaphoreGive(s_lock);
    return v;
}

float noise_floor_p95(void)
{
    if (!s_lock) return 0.0f;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    float v = percentile_locked(0.95f);
    xSemaphoreGive(s_lock);
    return v;
}

float noise_floor_threshold_adjust(void)
{
    if (!noise_floor_is_warm() || s_hist_total < 64) return 0.0f;
    float p95 = noise_floor_p95();
    /* Map p95 onto [0..margin]. Anchors: p95=500 → 0, p95=5000 → margin. */
    const float lo = 500.0f, hi = 5000.0f;
    if (p95 <= lo) return 0.0f;
    if (p95 >= hi) return s_margin;
    return s_margin * (p95 - lo) / (hi - lo);
}
