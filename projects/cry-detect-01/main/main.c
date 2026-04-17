#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "metrics.h"
#include "led_alert.h"
#include "audio_capture.h"
#include "mel_features.h"
#include "yamnet.h"
#include "detector.h"
#include "network.h"
#include "sd_logger.h"
#include "web_ui.h"
#include "noise_floor.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_yamnet_up;

#if CONFIG_CRY_STREAM_COMPILED_IN
#include "audio_stream.h"
#endif
#if CONFIG_CRY_REC_COMPILED_IN
#include "event_recorder.h"
#endif

static const char *TAG = "main";

static void on_net_state(bool wifi_up, bool ntp_synced, void *ctx)
{
    (void)ctx;
    static bool s_prev_wifi = false;
    static bool s_prev_ntp = false;

    metrics_set_wifi(wifi_up, 0);
    metrics_set_ntp_synced(ntp_synced);

    if (wifi_up && !s_prev_wifi) {
        sd_logger_event("wifi_up", 0.0f, 0);
    } else if (!wifi_up && s_prev_wifi) {
        sd_logger_event("wifi_down", 0.0f, 0);
    }
    if (ntp_synced && !s_prev_ntp) {
        sd_logger_ntp_sync_marker();
    }
    s_prev_wifi = wifi_up;
    s_prev_ntp = ntp_synced;

    if (!wifi_up) {
        led_alert_set(LED_STATE_CONNECTING);
        metrics_set_state(CRY_STATE_CONNECTING);
    } else if (!ntp_synced) {
        led_alert_set(LED_STATE_SYNCING);
        metrics_set_state(CRY_STATE_SYNCING);
    } else if (detector_get_state() != DETECTOR_CRYING) {
#if CONFIG_CRY_STREAM_COMPILED_IN
        led_alert_set(audio_stream_is_active() ? LED_STATE_STREAMING : LED_STATE_IDLE);
#else
        led_alert_set(LED_STATE_IDLE);
#endif
        metrics_set_state(CRY_STATE_IDLE);
    }
}

static void on_detector_state(detector_state_t new_state, float conf, void *ctx)
{
    (void)ctx;
    if (new_state == DETECTOR_CRYING) {
        led_alert_set(LED_STATE_ALERT);
        metrics_set_state(CRY_STATE_CRYING);
        metrics_increment_alert();

#if CONFIG_CRY_REC_COMPILED_IN
        const char *path = event_recorder_trigger(conf);
        const char *fname = path ? strrchr(path, '/') : NULL;
        if (fname) fname++; else fname = "";
#else
        const char *fname = "";
#endif

        char payload[160];
        snprintf(payload, sizeof(payload),
                 "{\"conf\":%.3f,\"ts\":%lld,\"wav\":\"%s\"}",
                 (double)conf,
                 (long long)(esp_timer_get_time() / 1000),
                 fname);
        web_ui_push_event("detect", payload);
        sd_logger_event("cry_start", conf, 0);
    } else {
#if CONFIG_CRY_STREAM_COMPILED_IN
        led_alert_set(audio_stream_is_active() ? LED_STATE_STREAMING : LED_STATE_IDLE);
#else
        led_alert_set(LED_STATE_IDLE);
#endif
        metrics_set_state(CRY_STATE_IDLE);
        sd_logger_event("cry_end", conf, 0);
    }
}

static esp_err_t mount_yamnet_spiffs(void)
{
    esp_vfs_spiffs_conf_t cfg = {
        .base_path = "/yamnet",
        .partition_label = "yamnet",
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    return esp_vfs_spiffs_register(&cfg);
}

static void inference_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "infer task: start (core=%d)", xPortGetCoreID());

    int16_t *pcm = heap_caps_malloc(MEL_HOP_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int8_t *patch = heap_caps_malloc(MEL_FRAMES_PATCH * MEL_BANDS, MALLOC_CAP_SPIRAM);
    if (!pcm || !patch) {
        ESP_LOGE(TAG, "inference alloc failed");
        vTaskDelete(NULL);
        return;
    }

    float scale = yamnet_input_scale();
    int zp = yamnet_input_zero_point();
    float base_threshold = 0.85f;  /* raised to mask FP bias from synthetic PTQ calibration */
    ESP_LOGI(TAG, "infer: input scale=%.5f zero_point=%d", (double)scale, zp);

    uint32_t loops = 0;
    uint32_t patches = 0;
    int64_t last_log_us = esp_timer_get_time();

    while (1) {
        size_t got = audio_capture_read(pcm, MEL_HOP_SAMPLES, pdMS_TO_TICKS(2000));
        if (got < MEL_HOP_SAMPLES) {
            ESP_LOGW(TAG, "infer: audio starved (%u/%u), looping", (unsigned)got, MEL_HOP_SAMPLES);
            continue;
        }
        loops++;

        int have_patch = mel_features_push(pcm, got);
        if (!have_patch) {
            /* heartbeat once per ~1 s so silence is never normal */
            int64_t now = esp_timer_get_time();
            if (now - last_log_us > 5000000) {
                ESP_LOGI(TAG, "infer: alive, loops=%u no patch yet", (unsigned)loops);
                last_log_us = now;
            }
            continue;
        }
        patches++;

        mel_features_take_patch(patch, scale, zp);

        yamnet_result_t r;
        esp_err_t rc = yamnet_run(patch, &r);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "infer: yamnet_run rc=0x%x", rc);
            continue;
        }

        metrics_update_inference(r.latency_ms, r.cry_conf);

#if CONFIG_CRY_NOISE_FLOOR_ENABLED
        detector_set_threshold(base_threshold + noise_floor_threshold_adjust());
#endif
        detector_submit(r.cry_conf);

        if ((patches & 0x7) == 0) {
            ESP_LOGI(TAG, "infer: #%u  ms=%d  cry_conf=%.3f  raw=%d  rms=%.0f  thr=%.2f",
                     (unsigned)patches, (int)r.latency_ms, (double)r.cry_conf,
                     (int)r.cry_raw_int8, 0.0, (double)detector_get_threshold());
        }

        char payload[96];
        snprintf(payload, sizeof(payload),
                 "{\"conf\":%.3f,\"ms\":%d}",
                 (double)r.cry_conf, (int)r.latency_ms);
        web_ui_push_event("inference", payload);
    }
}

/* Samples input RMS into noise_floor and keeps system metrics live.
 * Also emits periodic heartbeats to serial and SD so silence is a bug, not normal. */
static void housekeeping_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "hk task: start (core=%d)", xPortGetCoreID());
    int64_t next_serial_us = esp_timer_get_time() + 10 * 1000000;
    int64_t next_sd_us     = esp_timer_get_time() + 30 * 1000000;
    while (1) {
        cry_metrics_t m;
        metrics_snapshot(&m);
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
        noise_floor_submit_rms(m.input_rms);
#endif
        metrics_refresh_system();

        int64_t now = esp_timer_get_time();
        if (now >= next_serial_us) {
            next_serial_us = now + 10 * 1000000;
            metrics_snapshot(&m);
            ESP_LOGI(TAG, "hk: up=%us  st=%d  wifi=%d rssi=%d  ntp=%d  sd=%d  heap=%uKB  psram=%uKB  rms=%.0f  infer#=%u  last_ms=%d  fps=%.2f  cry=%.3f",
                     (unsigned)m.uptime_s, (int)m.state,
                     (int)m.wifi_connected, (int)m.wifi_rssi,
                     (int)m.ntp_synced, (int)m.sd_mounted,
                     (unsigned)(m.free_heap / 1024), (unsigned)(m.free_psram / 1024),
                     (double)m.input_rms,
                     (unsigned)m.inference_count, (int)m.last_inference_ms,
                     (double)m.inference_fps, (double)m.last_cry_conf);
        }
        if (now >= next_sd_us) {
            next_sd_us = now + 30 * 1000000;
            sd_logger_snapshot();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_ERROR_CHECK(nvs_flash_init());

    metrics_init();

    ESP_ERROR_CHECK(led_alert_init(CONFIG_CRY_DETECT_LED_EXPANDER_PIN));
    led_alert_set(LED_STATE_BOOT);
    vTaskDelay(pdMS_TO_TICKS(100));

    sd_logger_cfg_t lg = {
        .sd_enabled = true,
        .rotate_kb  = CONFIG_CRY_DETECT_LOG_ROTATE_KB,
    };
    sd_logger_init(&lg);
    metrics_set_sd_mounted(sd_logger_is_sd_mounted());
    sd_logger_event("boot", 0.0f, 0);

    ESP_ERROR_CHECK(mount_yamnet_spiffs());

    esp_err_t merr = yamnet_init(CONFIG_CRY_DETECT_MODEL_PATH, CONFIG_CRY_DETECT_TENSOR_ARENA_KB);
    s_yamnet_up = (merr == ESP_OK);
    if (!s_yamnet_up) {
        ESP_LOGE(TAG, "yamnet init failed: 0x%x -- running in INFRA-ONLY mode (Wi-Fi/mDNS/SD/UI up, no inference)", merr);
        metrics_set_state(CRY_STATE_ERROR);
        led_alert_set(LED_STATE_ERROR);
    }

    ESP_ERROR_CHECK(mel_features_init());
    ESP_ERROR_CHECK(audio_capture_init(CONFIG_CRY_DETECT_SAMPLE_RATE, CONFIG_CRY_DETECT_MIC_GAIN_DB));

    detector_init(0.85f, CONFIG_CRY_DETECT_CONSEC_FRAMES, CONFIG_CRY_DETECT_HOLD_MS,
                  on_detector_state, NULL);

#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    ESP_ERROR_CHECK(noise_floor_init(CONFIG_CRY_NOISE_FLOOR_WARMUP_S,
                                     (float)CONFIG_CRY_NOISE_FLOOR_MARGIN_X100 / 100.0f));
#endif
    xTaskCreatePinnedToCore(housekeeping_task, "hk", 6 * 1024, NULL, 2, NULL, 0);

#if CONFIG_CRY_STREAM_COMPILED_IN
    ESP_ERROR_CHECK(audio_stream_init(CONFIG_CRY_STREAM_MAX_LISTENERS,
                                      CONFIG_CRY_STREAM_RING_KB));
#endif

#if CONFIG_CRY_REC_COMPILED_IN
    event_recorder_cfg_t rec = {
        .preroll_s = CONFIG_CRY_REC_PREROLL_S,
        .postroll_s = CONFIG_CRY_REC_POSTROLL_S,
        .keep_files = CONFIG_CRY_REC_KEEP_FILES,
        .sample_rate = CONFIG_CRY_DETECT_SAMPLE_RATE,
        .mount_prefix = sd_logger_is_sd_mounted() ? "/sdcard" : "/logs",
        .subdir = "events",
    };
    event_recorder_init(&rec);
#endif

    /* network_start must run before web_ui_start so LWIP is initialized.
     * inference_task starts BEFORE web_ui_start, so web_ui_push_event
     * must NULL-guard its internal lock (see web_ui.c). */
    ESP_ERROR_CHECK(network_start("cry-detect-01", on_net_state, NULL));

    if (s_yamnet_up) {
        xTaskCreatePinnedToCore(inference_task, "infer", 8 * 1024, NULL, 5, NULL, 1);
    } else {
        ESP_LOGW(TAG, "skipping inference task: no model");
    }

#if CONFIG_CRY_DETECT_WEB_UI_ENABLED
    ESP_ERROR_CHECK(web_ui_start(CONFIG_CRY_DETECT_SSE_MAX_CLIENTS));
#endif

    if (s_yamnet_up) {
        led_alert_set(LED_STATE_IDLE);
    }  /* else LED stays in ERROR pattern set above */
    ESP_LOGI(TAG, "boot complete%s", s_yamnet_up ? "" : " (INFRA-ONLY)");
}
