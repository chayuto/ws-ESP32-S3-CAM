#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    CRY_STATE_BOOT = 0,
    CRY_STATE_CONNECTING,
    CRY_STATE_SYNCING,
    CRY_STATE_IDLE,
    CRY_STATE_CRYING,
    CRY_STATE_ERROR,
} cry_state_t;

typedef struct {
    cry_state_t state;
    bool ntp_synced;
    bool wifi_connected;
    bool sd_mounted;
    int8_t wifi_rssi;
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t free_psram;

    uint32_t inference_count;
    int32_t last_inference_ms;
    int32_t p95_inference_ms;
    float inference_fps;

    float last_cry_conf;
    float max_cry_conf_1s;
    uint32_t alert_count;
    time_t last_alert_epoch;

    float input_rms;
    uint32_t sse_clients;
    uint32_t log_bytes_written;
} cry_metrics_t;

void metrics_init(void);
void metrics_set_state(cry_state_t s);
void metrics_update_inference(int32_t latency_ms, float cry_conf);
void metrics_update_input_rms(float rms);
void metrics_increment_alert(void);
void metrics_set_ntp_synced(bool v);
void metrics_set_wifi(bool connected, int8_t rssi);
void metrics_set_sd_mounted(bool v);
void metrics_refresh_system(void);
void metrics_snapshot(cry_metrics_t *out);

typedef void (*metrics_event_cb_t)(const cry_metrics_t *snap, void *ctx);
void metrics_subscribe(metrics_event_cb_t cb, void *ctx);

size_t metrics_to_json(char *buf, size_t max_len);
