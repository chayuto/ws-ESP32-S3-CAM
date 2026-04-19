#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "metrics.h"
#include "audio_capture.h"
#include "mel_features.h"
#include "yamnet.h"
#include "detector.h"
#include "network.h"
#include "sd_logger.h"
#include "web_ui.h"
#include "noise_floor.h"
#include "metrics_logger.h"
#include "breadcrumb.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_yamnet_up;

/* P0 #1 fix (hygiene audit 2026-04-18): on_net_state runs on the
 * esp_event_loop_run_task; doing SD I/O there blocks the event loop long
 * enough to trip xQueueGiveMutexRecursive asserts. We post deferred work
 * to this queue and let housekeeping_task do the writes. */
typedef enum {
    DEFERRED_LOG_WIFI_UP = 0,
    DEFERRED_LOG_WIFI_DOWN,
    DEFERRED_LOG_NTP_SYNCED,
} deferred_log_evt_t;

static QueueHandle_t s_deferred_log_q;

#if CONFIG_CRY_STREAM_COMPILED_IN
#include "audio_stream.h"
#endif
#if CONFIG_CRY_REC_COMPILED_IN
#include "event_recorder.h"
#include "auto_trigger.h"
#include "log_retention.h"
#include "led_alert.h"
#endif

static const char *TAG = "main";

static void on_net_state(bool wifi_up, bool ntp_synced, void *ctx)
{
    (void)ctx;
    static bool s_prev_wifi = false;
    static bool s_prev_ntp = false;

    metrics_set_wifi(wifi_up, 0);
    metrics_set_ntp_synced(ntp_synced);

    /* Defer all SD I/O out of this callback (esp_event context). */
    if (s_deferred_log_q) {
        deferred_log_evt_t e;
        if (wifi_up && !s_prev_wifi) {
            e = DEFERRED_LOG_WIFI_UP;
            xQueueSend(s_deferred_log_q, &e, 0);
        } else if (!wifi_up && s_prev_wifi) {
            e = DEFERRED_LOG_WIFI_DOWN;
            xQueueSend(s_deferred_log_q, &e, 0);
        }
        if (ntp_synced && !s_prev_ntp) {
            e = DEFERRED_LOG_NTP_SYNCED;
            xQueueSend(s_deferred_log_q, &e, 0);
        }
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
    esp_task_wdt_add(NULL);

    int16_t *pcm = heap_caps_malloc(MEL_HOP_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int8_t *patch = heap_caps_malloc(MEL_FRAMES_PATCH * MEL_BANDS, MALLOC_CAP_SPIRAM);
    float *all_confs = heap_caps_malloc(521 * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!pcm || !patch || !all_confs) {
        ESP_LOGE(TAG, "inference alloc failed (pcm=%p patch=%p conf=%p)", pcm, patch, all_confs);
        free(pcm); free(patch); free(all_confs);   /* free(NULL) is safe */
        vTaskDelete(NULL);
        return;
    }

    float scale = yamnet_input_scale();
    int zp = yamnet_input_zero_point();
    /* 2026-04-18: dropped 0.85 → 0.65 after a 6.5 h deployment logged ZERO
     * alerts despite real crying events — threshold was above what the
     * synthetic-calibration bias allowed a true positive to reach. Paired
     * with CRY_DETECT_CONSEC_FRAMES=6 in Kconfig for stability against
     * single-frame spikes. Re-evaluate once Stage 2.1 real-audio
     * calibration lands. */
    /* 0.70 (sigmoid-of-dequant space). Replay harness against fixed-
     * logits model shows incident files saturate at 0.718, speech
     * bench files peak at 0.622. See training-epic-plan-20260419 §7a. */
    float base_threshold = 0.70f;
    ESP_LOGI(TAG, "infer: input scale=%.5f zero_point=%d num_classes=%d",
             (double)scale, zp, yamnet_num_classes());

    uint32_t loops = 0;
    uint32_t patches = 0;
    int64_t last_log_us = esp_timer_get_time();

    while (1) {
        esp_task_wdt_reset();

        size_t got = audio_capture_read(pcm, MEL_HOP_SAMPLES, pdMS_TO_TICKS(2000));
        if (got < MEL_HOP_SAMPLES) {
            ESP_LOGW(TAG, "infer: audio starved (%u/%u), looping", (unsigned)got, MEL_HOP_SAMPLES);
            continue;
        }
        loops++;

        int have_patch = mel_features_push(pcm, got);
        if (!have_patch) {
            int64_t now = esp_timer_get_time();
            if (now - last_log_us > 5000000) {
                ESP_LOGI(TAG, "infer: alive, loops=%u no patch yet", (unsigned)loops);
                last_log_us = now;
            }
            continue;
        }
        patches++;

        /* yamnet_run blocks the consumer for ~500 ms. During that time
         * the producer writes ~16 KB into the stream buffer. Without this
         * drain, each cycle leaves ~4 KB of backlog — the buffer fills and
         * starts dropping bytes after ~5 s of run time. Pre-yamnet drain:
         * read + mel_push any hops above half-full, so the next 500 ms of
         * producer fill lands on a near-empty ring. Latest patch is taken
         * after drain, so yamnet always runs on the most recent audio. */
        /* Drain to 1/4 not 1/2. With the fixed-dense model, yamnet_run
         * takes ~650 ms (vs ~500 ms before) — producer writes ~20 KB
         * per cycle, so leaving 16 KB in the ring pushes post-inference
         * total to ~36 KB > 32 KB capacity and we drop bytes. 1/4 leaves
         * 8 KB + 20 KB = 28 KB, fits with headroom. */
        const size_t drain_target = audio_capture_stream_capacity_bytes() / 4;
        unsigned drained_hops = 0;
        while (audio_capture_stream_bytes_available() > drain_target) {
            size_t dn = audio_capture_read(pcm, MEL_HOP_SAMPLES, 0);
            if (dn < MEL_HOP_SAMPLES) break;
            mel_features_push(pcm, dn);
            drained_hops++;
            if (drained_hops > 200) break;  /* belt-and-braces bound */
        }

        mel_features_take_patch(patch, scale, zp);

        yamnet_result_t r;
        esp_err_t rc = yamnet_run(patch, &r);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "infer: yamnet_run rc=0x%x", rc);
            continue;
        }

        yamnet_get_confidences(all_confs, 521);
        float watched[CRY_WATCHED_N];
        for (int i = 0; i < CRY_WATCHED_N; ++i) {
            watched[i] = all_confs[cry_watched_idx[i]];
        }
        metrics_update_watched(watched, CRY_WATCHED_N);

        metrics_update_inference(r.latency_ms, r.cry_conf);

        /* Publish full 521-class output to metrics_logger so its 1 Hz
         * JSONL row can compute top-10 over all AudioSet classes. */
        metrics_logger_publish_inference(all_confs, r.cry_conf, r.latency_ms);

#if CONFIG_CRY_NOISE_FLOOR_ENABLED
        detector_set_threshold(base_threshold + noise_floor_threshold_adjust());
#endif
        detector_submit(r.cry_conf);

        if ((patches & 0x7) == 0) {
            ESP_LOGI(TAG, "infer: #%u  ms=%d  cry=%.3f  laugh=%.3f  speech=%.3f  bark=%.3f  smoke=%.3f  thr=%.2f",
                     (unsigned)patches, (int)r.latency_ms,
                     (double)watched[0], (double)watched[7],
                     (double)watched[4], (double)watched[10],
                     (double)watched[19],
                     (double)detector_get_threshold());
        }

        char payload[128];
        snprintf(payload, sizeof(payload),
                 "{\"conf\":%.3f,\"ms\":%d,\"cry\":%.3f,\"laugh\":%.3f,\"smoke\":%.3f}",
                 (double)r.cry_conf, (int)r.latency_ms,
                 (double)watched[0], (double)watched[7], (double)watched[19]);
        web_ui_push_event("inference", payload);
    }
}

/* Samples input RMS into noise_floor and keeps system metrics live.
 * Also emits periodic heartbeats to serial and SD so silence is a bug, not normal. */
static void housekeeping_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "hk task: start (core=%d)", xPortGetCoreID());
    esp_task_wdt_add(NULL);
    int64_t next_serial_us = esp_timer_get_time() + 10 * 1000000;
    int64_t next_sd_us     = esp_timer_get_time() + 30 * 1000000;
    while (1) {
        esp_task_wdt_reset();

        /* Drain the deferred-log queue (posted by on_net_state from the
         * esp_event task). Safe to block on SD here. */
        if (s_deferred_log_q) {
            deferred_log_evt_t e;
            while (xQueueReceive(s_deferred_log_q, &e, 0) == pdTRUE) {
                switch (e) {
                    case DEFERRED_LOG_WIFI_UP:
                        sd_logger_event("wifi_up", 0.0f, 0);
                        breadcrumb_set("wifi_up");
                        break;
                    case DEFERRED_LOG_WIFI_DOWN:
                        sd_logger_event("wifi_down", 0.0f, 0);
                        breadcrumb_set("wifi_down");
                        break;
                    case DEFERRED_LOG_NTP_SYNCED:
                        sd_logger_ntp_sync_marker();
                        breadcrumb_set("ntp_synced");
                        break;
                }
            }
        }

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

    /* Read previous-boot breadcrumb BEFORE anything else can crash us.
     * If the last boot died during e.g. yamnet init, we'll see it here. */
    breadcrumb_init();

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
    breadcrumb_set("sd_mounted");

    ESP_ERROR_CHECK(mount_yamnet_spiffs());
    breadcrumb_set("spiffs_mounted");

    esp_err_t merr = yamnet_init(CONFIG_CRY_DETECT_MODEL_PATH, CONFIG_CRY_DETECT_TENSOR_ARENA_KB);
    s_yamnet_up = (merr == ESP_OK);
    if (!s_yamnet_up) {
        ESP_LOGE(TAG, "yamnet init failed: 0x%x -- running in INFRA-ONLY mode (Wi-Fi/mDNS/SD/UI up, no inference)", merr);
        metrics_set_state(CRY_STATE_ERROR);
        led_alert_set(LED_STATE_ERROR);
    }

    ESP_ERROR_CHECK(mel_features_init());
    ESP_ERROR_CHECK(audio_capture_init(CONFIG_CRY_DETECT_SAMPLE_RATE, CONFIG_CRY_DETECT_MIC_GAIN_DB));
    breadcrumb_set("audio_up");

    /* 0.70 is a cry_conf (sigmoid-of-dequant) threshold chosen from
     * replay_yamnet.py against the fixed-dense-layer model: incident
     * files saturate at 0.718, bench (speech) max at 0.622 — 0.70
     * sits in the middle with headroom on both sides. See
     * docs/research/training-epic-plan-20260419.md §7a. */
    detector_init(0.70f, CONFIG_CRY_DETECT_CONSEC_FRAMES, CONFIG_CRY_DETECT_HOLD_MS,
                  on_detector_state, NULL);

#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    ESP_ERROR_CHECK(noise_floor_init(CONFIG_CRY_NOISE_FLOOR_WARMUP_S,
                                     (float)CONFIG_CRY_NOISE_FLOOR_MARGIN_X100 / 100.0f));
#endif
    /* 8 KB stack: hk now drains the deferred-log queue (which calls
     * sd_logger_ntp_sync_marker → fopen/fwrite/fclose) on top of the
     * existing metrics/noise_floor hot path. Audit P1 #17. */
    xTaskCreatePinnedToCore(housekeeping_task, "hk", 8 * 1024, NULL, 2, NULL, 0);

    /* Verbose 1 Hz classification logger (Stage 2.6a). Runs in parallel
     * with sd_logger's CSV — JSONL is for analysis/retraining, CSV stays
     * human-readable. Re-enabled after P0 #1 fix (deferred SD I/O). */
    esp_err_t mlog_err = metrics_logger_init();
    if (mlog_err != ESP_OK) {
        ESP_LOGW(TAG, "metrics_logger init failed: 0x%x (non-fatal)", mlog_err);
    }

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

    /* RMS-threshold auto-trigger: overnight data-collection fallback while
     * the INT8 YAMNet head is saturated. Fires event_recorder_trigger_manual
     * on loud events, labeled auto-rms-Nx-rmsN for post-hoc culling. */
    ESP_ERROR_CHECK(auto_trigger_init());
#endif

    /* Periodic SD log pruner for infer-YYYYMMDD.jsonl / cry-YYYYMMDD.log.
     * Independent of the event recorder (WAV retention lives there).
     * Skips while NTP unsynced — no way to mis-date today's file. */
    ESP_ERROR_CHECK(log_retention_init());

    /* Create deferred-log queue before network starts so on_net_state's
     * first firing has somewhere to post (see P0 #1 in audit doc). */
    s_deferred_log_q = xQueueCreate(8, sizeof(deferred_log_evt_t));
    if (!s_deferred_log_q) {
        ESP_LOGE(TAG, "deferred-log queue create failed");
    }

    /* Start inference BEFORE network_start: example_connect's "wait for IP"
     * blocks ~7 s on a typical DHCP handshake, during which the audio
     * producer otherwise pumps 7 s × 32 KB/s straight into a ring the
     * consumer isn't draining yet. Infer has no Wi-Fi dependency. */
    if (s_yamnet_up) {
        xTaskCreatePinnedToCore(inference_task, "infer", 8 * 1024, NULL, 5, NULL, 1);
    } else {
        ESP_LOGW(TAG, "skipping inference task: no model");
    }

    /* network_start must run before web_ui_start so LWIP is initialized.
     * web_ui_push_event must NULL-guard its internal lock because infer is
     * already publishing by the time web_ui_start runs (see web_ui.c). */
    ESP_ERROR_CHECK(network_start("cry-detect-01", on_net_state, NULL));

#if CONFIG_CRY_DETECT_WEB_UI_ENABLED
    ESP_ERROR_CHECK(web_ui_start(CONFIG_CRY_DETECT_SSE_MAX_CLIENTS));
#endif

    if (s_yamnet_up) {
        led_alert_set(LED_STATE_IDLE);
    }
    breadcrumb_set(s_yamnet_up ? "run" : "run_infra_only");
    ESP_LOGI(TAG, "boot complete%s", s_yamnet_up ? "" : " (INFRA-ONLY)");
}
