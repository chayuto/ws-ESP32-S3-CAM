#include "metrics.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#if CONFIG_CRY_NOISE_FLOOR_ENABLED
#include "noise_floor.h"
#endif
#if CONFIG_CRY_STREAM_COMPILED_IN
#include "audio_stream.h"
#endif

static const char *TAG = "metrics";

#define P95_HISTORY 64
#define MAX_SUBSCRIBERS 4

typedef struct {
    metrics_event_cb_t cb;
    void *ctx;
} subscriber_t;

static cry_metrics_t s_metrics;
static SemaphoreHandle_t s_lock;
static int32_t s_latency_hist[P95_HISTORY];
static uint32_t s_latency_hist_pos;
static int64_t s_last_inference_us;
static subscriber_t s_subs[MAX_SUBSCRIBERS];
static uint32_t s_sub_count;

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

void metrics_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_metrics.state = CRY_STATE_BOOT;
    s_last_inference_us = 0;
    s_latency_hist_pos = 0;
    s_sub_count = 0;
}

void metrics_set_state(cry_state_t s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.state = s;
    xSemaphoreGive(s_lock);
}

static void fanout_locked(void)
{
    cry_metrics_t snap = s_metrics;
    for (uint32_t i = 0; i < s_sub_count; ++i) {
        subscriber_t sub = s_subs[i];
        xSemaphoreGive(s_lock);
        sub.cb(&snap, sub.ctx);
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void metrics_update_inference(int32_t latency_ms, float cry_conf)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.inference_count++;
    s_metrics.last_inference_ms = latency_ms;
    s_latency_hist[s_latency_hist_pos++ % P95_HISTORY] = latency_ms;

    int32_t sorted[P95_HISTORY];
    uint32_t n = s_metrics.inference_count < P95_HISTORY ? s_metrics.inference_count : P95_HISTORY;
    memcpy(sorted, s_latency_hist, n * sizeof(int32_t));
    qsort(sorted, n, sizeof(int32_t), cmp_i32);
    s_metrics.p95_inference_ms = sorted[(n * 95) / 100];

    int64_t now = esp_timer_get_time();
    if (s_last_inference_us > 0) {
        int64_t dt_us = now - s_last_inference_us;
        if (dt_us > 0) {
            float inst_fps = 1e6f / (float)dt_us;
            s_metrics.inference_fps = 0.8f * s_metrics.inference_fps + 0.2f * inst_fps;
        }
    }
    s_last_inference_us = now;

    s_metrics.last_cry_conf = cry_conf;
    if (cry_conf > s_metrics.max_cry_conf_1s) {
        s_metrics.max_cry_conf_1s = cry_conf;
    } else {
        s_metrics.max_cry_conf_1s = 0.95f * s_metrics.max_cry_conf_1s;
    }

    s_metrics.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_metrics.free_heap = esp_get_free_heap_size();
    s_metrics.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_metrics.wifi_rssi = ap.rssi;
        s_metrics.wifi_connected = true;
    } else {
        s_metrics.wifi_connected = false;
    }

    fanout_locked();
    xSemaphoreGive(s_lock);
}

void metrics_update_input_rms(float rms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.input_rms = rms;
    xSemaphoreGive(s_lock);
}

void metrics_increment_alert(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.alert_count++;
    time_t now;
    time(&now);
    s_metrics.last_alert_epoch = now;
    fanout_locked();
    xSemaphoreGive(s_lock);
}

void metrics_set_ntp_synced(bool v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.ntp_synced = v;
    xSemaphoreGive(s_lock);
}

void metrics_set_wifi(bool connected, int8_t rssi)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.wifi_connected = connected;
    s_metrics.wifi_rssi = rssi;
    xSemaphoreGive(s_lock);
}

void metrics_set_sd_mounted(bool v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.sd_mounted = v;
    xSemaphoreGive(s_lock);
}

void metrics_refresh_system(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_metrics.free_heap = esp_get_free_heap_size();
    s_metrics.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_metrics.wifi_rssi = ap.rssi;
        s_metrics.wifi_connected = true;
    }
    xSemaphoreGive(s_lock);
}

void metrics_snapshot(cry_metrics_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_metrics;
    xSemaphoreGive(s_lock);
}

void metrics_subscribe(metrics_event_cb_t cb, void *ctx)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_sub_count < MAX_SUBSCRIBERS) {
        s_subs[s_sub_count++] = (subscriber_t){ .cb = cb, .ctx = ctx };
    } else {
        ESP_LOGW(TAG, "subscriber slot full, drop");
    }
    xSemaphoreGive(s_lock);
}

static const char *state_str(cry_state_t s)
{
    switch (s) {
        case CRY_STATE_BOOT:       return "boot";
        case CRY_STATE_CONNECTING: return "connecting";
        case CRY_STATE_SYNCING:    return "syncing";
        case CRY_STATE_IDLE:       return "idle";
        case CRY_STATE_CRYING:     return "crying";
        case CRY_STATE_ERROR:      return "error";
        default:                   return "unknown";
    }
}

size_t metrics_to_json(char *buf, size_t max_len)
{
    cry_metrics_t m;
    metrics_snapshot(&m);

    float nf_p50 = 0, nf_p95 = 0;
    bool nf_warm = false;
    uint32_t nf_rem = 0;
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    nf_p50 = noise_floor_p50();
    nf_p95 = noise_floor_p95();
    nf_warm = noise_floor_is_warm();
    nf_rem = noise_floor_remaining_warmup_s();
#endif

    uint32_t listeners = 0;
    bool stream_enabled = false;
#if CONFIG_CRY_STREAM_COMPILED_IN
    listeners = audio_stream_listener_count();
    stream_enabled = true;
#endif

    int n = snprintf(buf, max_len,
        "{\"state\":\"%s\",\"ntp_synced\":%s,\"wifi_connected\":%s,"
        "\"wifi_rssi\":%d,\"sd_mounted\":%s,\"uptime_s\":%u,"
        "\"free_heap\":%u,\"free_psram\":%u,"
        "\"inference_count\":%u,\"last_inference_ms\":%d,\"p95_inference_ms\":%d,"
        "\"inference_fps\":%.2f,"
        "\"last_cry_conf\":%.3f,\"max_cry_conf_1s\":%.3f,"
        "\"alert_count\":%u,\"last_alert_epoch\":%lld,"
        "\"input_rms\":%.1f,\"sse_clients\":%u,\"log_bytes_written\":%u,"
        "\"noise_floor_p50\":%.1f,\"noise_floor_p95\":%.1f,"
        "\"noise_floor_warm\":%s,\"noise_floor_remaining_s\":%u,"
        "\"stream_enabled\":%s,\"stream_listeners\":%u}",
        state_str(m.state),
        m.ntp_synced ? "true" : "false",
        m.wifi_connected ? "true" : "false",
        (int)m.wifi_rssi,
        m.sd_mounted ? "true" : "false",
        (unsigned)m.uptime_s,
        (unsigned)m.free_heap, (unsigned)m.free_psram,
        (unsigned)m.inference_count, (int)m.last_inference_ms, (int)m.p95_inference_ms,
        (double)m.inference_fps,
        (double)m.last_cry_conf, (double)m.max_cry_conf_1s,
        (unsigned)m.alert_count, (long long)m.last_alert_epoch,
        (double)m.input_rms,
        (unsigned)m.sse_clients, (unsigned)m.log_bytes_written,
        (double)nf_p50, (double)nf_p95,
        nf_warm ? "true" : "false", (unsigned)nf_rem,
        stream_enabled ? "true" : "false", (unsigned)listeners);
    return n < 0 ? 0 : (size_t)n;
}
