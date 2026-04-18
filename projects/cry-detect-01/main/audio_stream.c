#include "audio_stream.h"

#include <string.h>
#include <stdatomic.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "audio_capture.h"
#include "metrics.h"

static const char *TAG = "astream";

static uint32_t s_max_listeners;
static uint32_t s_ring_bytes;
/* Atomic so two concurrent listener-connects can't both pass the capacity
 * check and corrupt the count (hygiene audit P0 #8). */
static atomic_uint s_listener_count;

esp_err_t audio_stream_init(uint32_t max_listeners, uint32_t ring_kb)
{
    s_max_listeners = max_listeners;
    s_ring_bytes    = ring_kb * 1024u;
    atomic_store(&s_listener_count, 0);
    ESP_LOGI(TAG, "init: max=%u ring=%u KB", (unsigned)max_listeners, (unsigned)ring_kb);
    return ESP_OK;
}

uint32_t audio_stream_listener_count(void)
{
    return atomic_load(&s_listener_count);
}

bool audio_stream_is_active(void)
{
    return atomic_load(&s_listener_count) > 0;
}

esp_err_t audio_stream_http_handler(httpd_req_t *req)
{
    /* Reserve a slot atomically so two concurrent connects can't both pass. */
    uint32_t prev = atomic_fetch_add(&s_listener_count, 1);
    if (prev >= s_max_listeners) {
        atomic_fetch_sub(&s_listener_count, 1);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "stream capacity full", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    audio_tap_handle_t tap = audio_capture_add_tap(s_ring_bytes);
    if (!tap) {
        atomic_fetch_sub(&s_listener_count, 1);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "tap alloc failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "listener connected, count=%u", (unsigned)atomic_load(&s_listener_count));

    httpd_resp_set_type(req, "audio/L16; rate=16000; channels=1");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    /* Stream PCM until the client disconnects. 3200 samples = 200 ms chunk. */
    const size_t chunk_samples = 3200;
    int16_t *buf = heap_caps_malloc(chunk_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        audio_capture_remove_tap(tap);
        atomic_fetch_sub(&s_listener_count, 1);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    while (1) {
        size_t got = audio_capture_tap_read(tap, buf, chunk_samples, pdMS_TO_TICKS(1000));
        if (got == 0) continue;
        err = httpd_resp_send_chunk(req, (const char *)buf, got * sizeof(int16_t));
        if (err != ESP_OK) break;
    }

    free(buf);
    audio_capture_remove_tap(tap);
    atomic_fetch_sub(&s_listener_count, 1);
    ESP_LOGI(TAG, "listener disconnected, count=%u", (unsigned)atomic_load(&s_listener_count));
    return err;
}
