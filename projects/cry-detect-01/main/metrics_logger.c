#include "metrics_logger.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "driver/temperature_sensor.h"

#include "metrics.h"
#include "sd_logger.h"
#include "network.h"
#include "sdkconfig.h"

#if CONFIG_CRY_NOISE_FLOOR_ENABLED
#include "noise_floor.h"
#endif

static const char *TAG = "mlog";

#define YAMNET_CLASSES  521
#define TOPK            10
#define STACK_MAX       20

static SemaphoreHandle_t s_lock;
static float *s_last_conf;           /* [521] guarded by s_lock, PSRAM */
static float  s_last_cry_conf;
static int32_t s_last_latency_ms;
static uint32_t s_last_seq;          /* monotonic publish counter */

static FILE *s_f;
static char  s_path[64];
static int   s_day_of_year;
static const char *s_mount_prefix;   /* /sdcard or /logs */

static temperature_sensor_handle_t s_tsens;

static const char *state_name(cry_state_t s)
{
    switch (s) {
        case CRY_STATE_BOOT:       return "boot";
        case CRY_STATE_CONNECTING: return "connecting";
        case CRY_STATE_SYNCING:    return "syncing";
        case CRY_STATE_IDLE:       return "idle";
        case CRY_STATE_CRYING:     return "crying";
        case CRY_STATE_ERROR:      return "error";
        default:                   return "?";
    }
}

/* Same format as sd_logger.c: RFC-3339 local time with numeric offset.
 * Kept duplicated to avoid a refactor; consolidate in a util if a third
 * caller needs it. */
static int format_timestamp(char *buf, size_t max)
{
    if (network_is_ntp_synced()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tmv;
        localtime_r(&tv.tv_sec, &tmv);
        int n = strftime(buf, max, "%Y-%m-%dT%H:%M:%S", &tmv);
        n += snprintf(buf + n, max - n, ".%03ld", tv.tv_usec / 1000);
        int m = strftime(buf + n, max - n, "%z", &tmv);
        if (m == 5 && (buf[n] == '+' || buf[n] == '-') && (size_t)(n + 7) < max) {
            memmove(buf + n + 4, buf + n + 3, 2);
            buf[n + 3] = ':';
            m++;
            buf[n + m] = '\0';
        }
        return n + m;
    } else {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        return snprintf(buf, max, "up=%us,NOT_SYNCED", (unsigned)up);
    }
}

static void make_path(char *out, size_t max)
{
    if (network_is_ntp_synced()) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char day[16];
        strftime(day, sizeof(day), "%Y%m%d", &tmv);
        snprintf(out, max, "%s/infer-%s.jsonl", s_mount_prefix, day);
    } else {
        /* Pre-NTP: go to a boot-local file so rows still land somewhere. */
        snprintf(out, max, "%s/infer-boot.jsonl", s_mount_prefix);
    }
}

static void reopen_if_needed(void)
{
    struct tm tmv;
    time_t now = time(NULL);
    localtime_r(&now, &tmv);

    bool need = (s_f == NULL) || (network_is_ntp_synced() && tmv.tm_yday != s_day_of_year);
    if (!need) return;

    if (s_f) { fclose(s_f); s_f = NULL; }
    make_path(s_path, sizeof(s_path));
    s_f = fopen(s_path, "a");
    s_day_of_year = tmv.tm_yday;
    if (!s_f) {
        ESP_LOGW(TAG, "fopen %s failed", s_path);
    } else {
        ESP_LOGI(TAG, "jsonl logging to %s", s_path);
    }
}

/* Find top-K indices by confidence in a 521-float array.
 * O(n*k) — fine for n=521, k=10 at 1 Hz. */
static void find_topk(const float *confs, int n, int k, int *idx_out, float *val_out)
{
    for (int i = 0; i < k; ++i) { idx_out[i] = -1; val_out[i] = -1.0f; }
    for (int i = 0; i < n; ++i) {
        float v = confs[i];
        /* find the smallest in idx_out and replace if this is larger */
        int min_slot = 0;
        for (int j = 1; j < k; ++j) if (val_out[j] < val_out[min_slot]) min_slot = j;
        if (v > val_out[min_slot]) {
            val_out[min_slot] = v;
            idx_out[min_slot] = i;
        }
    }
    /* Sort descending by val (simple insertion sort, k=10). */
    for (int i = 1; i < k; ++i) {
        float v = val_out[i]; int idx = idx_out[i];
        int j = i - 1;
        while (j >= 0 && val_out[j] < v) {
            val_out[j + 1] = val_out[j];
            idx_out[j + 1] = idx_out[j];
            j--;
        }
        val_out[j + 1] = v;
        idx_out[j + 1] = idx;
    }
}

static void write_row(FILE *f)
{
    char ts[48];
    format_timestamp(ts, sizeof(ts));
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);

    cry_metrics_t m;
    metrics_snapshot(&m);

    float nf_p50 = 0, nf_p95 = 0;
    bool nf_warm = false;
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    nf_p50 = noise_floor_p50();
    nf_p95 = noise_floor_p95();
    nf_warm = noise_floor_is_warm();
#endif

    /* Snapshot shared inference state under lock. */
    static float conf_copy[YAMNET_CLASSES];
    float cry_conf, lat_ms;
    uint32_t seq;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(conf_copy, s_last_conf, YAMNET_CLASSES * sizeof(float));
    cry_conf = s_last_cry_conf;
    lat_ms   = (float)s_last_latency_ms;
    seq      = s_last_seq;
    xSemaphoreGive(s_lock);

    int top_idx[TOPK]; float top_val[TOPK];
    find_topk(conf_copy, YAMNET_CLASSES, TOPK, top_idx, top_val);

    float die_c = -273.15f;
    if (s_tsens) (void)temperature_sensor_get_celsius(s_tsens, &die_c);

    /* Emit. 2 KB buffer is plenty for ~1 KB of JSON. */
    char buf[2048];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
        "{\"ts\":\"%s\",\"up\":%u,\"n_infer\":%u,\"n_pub\":%u,\"state\":\"%s\"",
        ts, (unsigned)up, (unsigned)m.inference_count, (unsigned)seq,
        state_name(m.state));

    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"audio\":{\"rms\":%.1f,\"nf_p50\":%.1f,\"nf_p95\":%.1f,\"nf_warm\":%s}",
        (double)m.input_rms, (double)nf_p50, (double)nf_p95,
        nf_warm ? "true" : "false");

    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"infer\":{\"ms\":%d,\"fps\":%.2f,\"cry_conf\":%.3f,\"max_cry_1s\":%.3f,\"top10\":[",
        (int)lat_ms, (double)m.inference_fps,
        (double)cry_conf, (double)m.max_cry_conf_1s);
    for (int i = 0; i < TOPK; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"i\":%d,\"v\":%.3f}",
                      i ? "," : "", top_idx[i], (double)top_val[i]);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");

    /* 20 watched class confidences as a positional array — names are in
     * metrics.c cry_watched_name[] and index-stable; offline join. */
    n += snprintf(buf + n, sizeof(buf) - n, ",\"watched\":[");
    for (int i = 0; i < CRY_WATCHED_N; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s%.3f",
                      i ? "," : "", (double)m.watched_conf[i]);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]");

    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"sys\":{\"free_heap\":%u,\"free_psram\":%u,"
        "\"min_heap\":%u,\"rssi\":%d,\"die_c\":%.1f,"
        "\"ntp\":%s,\"wifi\":%s,\"sd\":%s,\"alerts\":%u}",
        (unsigned)m.free_heap, (unsigned)m.free_psram,
        (unsigned)esp_get_minimum_free_heap_size(),
        (int)m.wifi_rssi, (double)die_c,
        m.ntp_synced ? "true" : "false",
        m.wifi_connected ? "true" : "false",
        m.sd_mounted ? "true" : "false",
        (unsigned)m.alert_count);

    /* Stack HWM per task — uses uxTaskGetSystemState (trace facility). */
#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t ntasks = uxTaskGetNumberOfTasks();
    if (ntasks > STACK_MAX) ntasks = STACK_MAX;
    TaskStatus_t *ts_arr = heap_caps_malloc(ntasks * sizeof(TaskStatus_t),
                                            MALLOC_CAP_INTERNAL);
    if (ts_arr) {
        UBaseType_t got = uxTaskGetSystemState(ts_arr, ntasks, NULL);
        n += snprintf(buf + n, sizeof(buf) - n, ",\"stacks\":[");
        bool first = true;
        for (UBaseType_t i = 0; i < got && n < (int)sizeof(buf) - 64; ++i) {
            n += snprintf(buf + n, sizeof(buf) - n,
                          "%s{\"t\":\"%.12s\",\"hwm\":%u}",
                          first ? "" : ",",
                          ts_arr[i].pcTaskName ? ts_arr[i].pcTaskName : "?",
                          (unsigned)ts_arr[i].usStackHighWaterMark);
            first = false;
        }
        n += snprintf(buf + n, sizeof(buf) - n, "]");
        free(ts_arr);
    }
#endif

    n += snprintf(buf + n, sizeof(buf) - n, "}\n");

    if (n > 0 && n < (int)sizeof(buf) && f) {
        fwrite(buf, 1, n, f);
    }
}

static void logger_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "metrics_logger task: start (core=%d)", xPortGetCoreID());

    TickType_t last = xTaskGetTickCount();
    uint32_t ticks = 0;
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        reopen_if_needed();
        if (s_f) {
            write_row(s_f);
            /* fflush alone leaves data in the FATFS cache — fsync commits
             * to SD. Without fsync, a crash loses every row since the last
             * fclose, which only happens at day rollover. */
            if (++ticks % 10 == 0) {
                fflush(s_f);
                fsync(fileno(s_f));
            }
        }
    }
}

void metrics_logger_publish_inference(const float *conf_521,
                                      float cry_conf,
                                      int32_t latency_ms)
{
    if (!s_lock || !s_last_conf) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_last_conf, conf_521, YAMNET_CLASSES * sizeof(float));
    s_last_cry_conf = cry_conf;
    s_last_latency_ms = latency_ms;
    s_last_seq++;
    xSemaphoreGive(s_lock);
}

esp_err_t metrics_logger_init(void)
{
    s_mount_prefix = sd_logger_is_sd_mounted() ? "/sdcard" : "/logs";

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_last_conf = heap_caps_calloc(YAMNET_CLASSES, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_last_conf) return ESP_ERR_NO_MEM;

    /* ESP32-S3 die-temp sensor: range wide enough for fanless boards. */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    esp_err_t er = temperature_sensor_install(&cfg, &s_tsens);
    if (er == ESP_OK) {
        er = temperature_sensor_enable(s_tsens);
    }
    if (er != ESP_OK) {
        ESP_LOGW(TAG, "die-temp sensor install/enable failed: 0x%x (non-fatal)", er);
        s_tsens = NULL;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        logger_task, "mlog", 6 * 1024, NULL, 2, NULL, 0);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "init: prefix=%s die_temp=%s", s_mount_prefix,
             s_tsens ? "ok" : "unavailable");
    return ESP_OK;
}
