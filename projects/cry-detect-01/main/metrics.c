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
#include "esp_app_desc.h"
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

/* Curated watched-class table. Indices from Google AudioSet / YAMNet
 * class map. Names are short for log-column headers. Order is
 * fixed: changing this column order breaks downstream SD-log readers. */
const int cry_watched_idx[CRY_WATCHED_N] = {
    20, 19, 21, 22,                  /* cry spectrum */
    0,  1,  13, 14, 15,              /* context (speech + joy) */
    65, 70, 78, 42, 44,              /* FP sources (babble, bark, meow, cough, sneeze) */
    371, 406,                        /* appliance noise (vacuum, fan) */
    11, 389, 390, 393,               /* urgent (scream, alarm clock, siren, smoke alarm) */
};
const char *const cry_watched_name[CRY_WATCHED_N] = {
    "cry_baby", "cry_adult", "whimper", "wail_moan",
    "speech", "child_speech", "laughter", "baby_laughter", "giggle",
    "hubbub", "dog_bark", "meow", "cough", "sneeze",
    "vacuum", "fan",
    "scream", "alarm_clock", "siren", "smoke_alarm",
};
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

/* Must be called with s_lock held. Returns with s_lock held.
 *
 * Copies the subscriber table and metrics snapshot to local storage, drops
 * the lock for the entire fanout, then re-acquires. Previous implementation
 * gave/took the lock inside the loop per-subscriber, which (a) paid the
 * mutex cost N times for no isolation benefit, and (b) let s_sub_count
 * grow mid-iteration — a late-subscribing thread would see the callback
 * table between give/take in an indeterminate state. With a local copy the
 * fanout is strictly ordered against its own captured snapshot, and
 * subscribers added during dispatch get events on the next update. */
static void fanout_locked(void)
{
    cry_metrics_t snap = s_metrics;
    subscriber_t subs_copy[MAX_SUBSCRIBERS];
    uint32_t n = s_sub_count;
    if (n > MAX_SUBSCRIBERS) n = MAX_SUBSCRIBERS;
    for (uint32_t i = 0; i < n; ++i) subs_copy[i] = s_subs[i];
    xSemaphoreGive(s_lock);
    for (uint32_t i = 0; i < n; ++i) {
        if (subs_copy[i].cb) subs_copy[i].cb(&snap, subs_copy[i].ctx);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
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

void metrics_increment_sd_write_error(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.sd_write_errors++;
    xSemaphoreGive(s_lock);
}

void metrics_add_audio_overrun(uint32_t bytes_dropped)
{
    if (!s_lock || bytes_dropped == 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_metrics.audio_overrun_bytes += bytes_dropped;
    s_metrics.audio_overrun_events++;
    xSemaphoreGive(s_lock);
}

void metrics_update_watched(const float *confs, int n)
{
    if (!confs) return;
    if (n > CRY_WATCHED_N) n = CRY_WATCHED_N;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < n; ++i) s_metrics.watched_conf[i] = confs[i];
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

    /* Build identity — ESP-IDF embeds date/time/git hash in app_desc. */
    const esp_app_desc_t *ad = esp_app_get_description();
    char build_sha[9] = "";   /* first 8 hex chars of elf_sha256 */
    if (ad) {
        for (int i = 0; i < 4; ++i) {
            snprintf(build_sha + i*2, 3, "%02x", ad->app_elf_sha256[i]);
        }
    }

    int n = snprintf(buf, max_len,
        "{\"state\":\"%s\",\"ntp_synced\":%s,\"wifi_connected\":%s,"
        "\"wifi_rssi\":%d,\"sd_mounted\":%s,\"uptime_s\":%u,"
        "\"free_heap\":%u,\"free_psram\":%u,"
        "\"inference_count\":%u,\"last_inference_ms\":%d,\"p95_inference_ms\":%d,"
        "\"inference_fps\":%.2f,"
        "\"last_cry_conf\":%.3f,\"max_cry_conf_1s\":%.3f,"
        "\"alert_count\":%u,\"last_alert_epoch\":%lld,"
        "\"input_rms\":%.1f,\"sse_clients\":%u,\"log_bytes_written\":%u,"
        "\"sd_write_errors\":%u,"
        "\"audio_overrun_bytes\":%u,\"audio_overrun_events\":%u,"
        "\"noise_floor_p50\":%.1f,\"noise_floor_p95\":%.1f,"
        "\"noise_floor_warm\":%s,\"noise_floor_remaining_s\":%u,"
        "\"stream_enabled\":%s,\"stream_listeners\":%u,"
        "\"build_date\":\"%s\",\"build_time\":\"%s\","
        "\"build_sha\":\"%s\",\"fw_ver\":\"%s\","
        "\"watched\":{",
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
        (unsigned)m.sd_write_errors,
        (unsigned)m.audio_overrun_bytes, (unsigned)m.audio_overrun_events,
        (double)nf_p50, (double)nf_p95,
        nf_warm ? "true" : "false", (unsigned)nf_rem,
        stream_enabled ? "true" : "false", (unsigned)listeners,
        ad ? ad->date : "?",
        ad ? ad->time : "?",
        build_sha,
        ad ? ad->version : "?");
    if (n < 0) return 0;

    for (int i = 0; i < CRY_WATCHED_N && n < (int)max_len - 40; ++i) {
        n += snprintf(buf + n, max_len - n, "%s\"%s\":%.3f",
                      i ? "," : "", cry_watched_name[i], (double)m.watched_conf[i]);
    }
    n += snprintf(buf + n, max_len - n, "}}");
    return (size_t)n;
}
