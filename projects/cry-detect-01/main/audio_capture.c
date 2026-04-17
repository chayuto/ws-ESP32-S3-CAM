#include "audio_capture.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

#include "metrics.h"

static const char *TAG = "audio";

#define FRAME_SAMPLES 160                 /* 10 ms @ 16 kHz */
#define STREAM_BYTES  (4 * 1024 * sizeof(int16_t))
#define READ_CHUNK_SAMPLES (FRAME_SAMPLES * 4)   /* 40 ms read granularity */
#define MAX_TAPS 4

typedef struct {
    StreamBufferHandle_t ring;
    bool in_use;
} tap_slot_t;

static esp_codec_dev_handle_t s_mic;
static StreamBufferHandle_t s_stream;
static TaskHandle_t s_task;

static tap_slot_t s_taps[MAX_TAPS];
static SemaphoreHandle_t s_taps_lock;

StreamBufferHandle_t audio_capture_stream(void)
{
    return s_stream;
}

static float compute_rms(const int16_t *s, size_t n)
{
    if (n == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double v = (double)s[i];
        acc += v * v;
    }
    return (float)sqrt(acc / (double)n);
}

static void fanout_taps(const int16_t *pcm, size_t samples)
{
    xSemaphoreTake(s_taps_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_TAPS; ++i) {
        if (s_taps[i].in_use) {
            xStreamBufferSend(s_taps[i].ring, pcm, samples * sizeof(int16_t), 0);
        }
    }
    xSemaphoreGive(s_taps_lock);
}

static void capture_task(void *arg)
{
    int16_t *buf = heap_caps_malloc(READ_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "alloc read buffer failed");
        vTaskDelete(NULL);
        return;
    }
    esp_task_wdt_add(NULL);

    uint32_t rms_downsample = 0;
    while (1) {
        esp_task_wdt_reset();
        int rc = esp_codec_dev_read(s_mic, buf, READ_CHUNK_SAMPLES * sizeof(int16_t));
        if (rc != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec read rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t wanted = READ_CHUNK_SAMPLES * sizeof(int16_t);
        size_t sent = xStreamBufferSend(s_stream, buf, wanted, pdMS_TO_TICKS(20));
        if (sent != wanted) {
            ESP_LOGW(TAG, "stream overrun: %u/%u", (unsigned)sent, (unsigned)wanted);
        }
        fanout_taps(buf, READ_CHUNK_SAMPLES);

        if ((rms_downsample++ & 0xF) == 0) {
            metrics_update_input_rms(compute_rms(buf, READ_CHUNK_SAMPLES));
        }
    }
}

esp_err_t audio_capture_init(uint32_t sample_rate_hz, int mic_gain_db)
{
    ESP_ERROR_CHECK(bsp_i2c_init());
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "mic init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = sample_rate_hz,
        .channel         = 1,
        .bits_per_sample = 16,
    };
    esp_codec_dev_set_in_gain(s_mic, (float)mic_gain_db);
    int rc = esp_codec_dev_open(s_mic, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open rc=%d", rc);
        return ESP_FAIL;
    }

    s_stream = xStreamBufferCreate(STREAM_BYTES, FRAME_SAMPLES * sizeof(int16_t));
    s_taps_lock = xSemaphoreCreateMutex();
    if (!s_stream || !s_taps_lock) {
        ESP_LOGE(TAG, "stream/lock create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        capture_task, "audio_cap", 4 * 1024, NULL, 10, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task start failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "audio capture up: %u Hz, gain %d dB", (unsigned)sample_rate_hz, mic_gain_db);
    return ESP_OK;
}

size_t audio_capture_read(int16_t *dst, size_t want_samples, TickType_t timeout)
{
    size_t got = xStreamBufferReceive(s_stream, dst, want_samples * sizeof(int16_t), timeout);
    return got / sizeof(int16_t);
}

audio_tap_handle_t audio_capture_add_tap(size_t ring_bytes)
{
    xSemaphoreTake(s_taps_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_TAPS; ++i) {
        if (!s_taps[i].in_use) {
            s_taps[i].ring = xStreamBufferCreate(ring_bytes, FRAME_SAMPLES * sizeof(int16_t));
            if (!s_taps[i].ring) {
                xSemaphoreGive(s_taps_lock);
                return NULL;
            }
            s_taps[i].in_use = true;
            xSemaphoreGive(s_taps_lock);
            return (audio_tap_handle_t)(intptr_t)(i + 1);
        }
    }
    xSemaphoreGive(s_taps_lock);
    return NULL;
}

void audio_capture_remove_tap(audio_tap_handle_t h)
{
    int idx = (int)(intptr_t)h - 1;
    if (idx < 0 || idx >= MAX_TAPS) return;
    xSemaphoreTake(s_taps_lock, portMAX_DELAY);
    if (s_taps[idx].in_use) {
        StreamBufferHandle_t b = s_taps[idx].ring;
        s_taps[idx].in_use = false;
        s_taps[idx].ring = NULL;
        xSemaphoreGive(s_taps_lock);
        vStreamBufferDelete(b);
        return;
    }
    xSemaphoreGive(s_taps_lock);
}

size_t audio_capture_tap_read(audio_tap_handle_t h, int16_t *dst, size_t max_samples, TickType_t timeout)
{
    int idx = (int)(intptr_t)h - 1;
    if (idx < 0 || idx >= MAX_TAPS || !s_taps[idx].in_use) return 0;
    size_t got = xStreamBufferReceive(s_taps[idx].ring, dst, max_samples * sizeof(int16_t), timeout);
    return got / sizeof(int16_t);
}

size_t audio_capture_tap_available(audio_tap_handle_t h)
{
    int idx = (int)(intptr_t)h - 1;
    if (idx < 0 || idx >= MAX_TAPS || !s_taps[idx].in_use) return 0;
    return xStreamBufferBytesAvailable(s_taps[idx].ring) / sizeof(int16_t);
}
