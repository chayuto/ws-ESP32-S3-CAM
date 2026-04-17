#include "event_recorder.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "audio_capture.h"
#include "network.h"

static const char *TAG = "rec";

static event_recorder_cfg_t s_cfg;
static char s_event_dir[48];
static audio_tap_handle_t s_tap;

static int16_t *s_preroll;
static size_t s_preroll_samples;
static size_t s_preroll_head;
static size_t s_preroll_filled;

static TaskHandle_t s_task;
static volatile bool s_recording;
static volatile float s_trigger_conf;

static SemaphoreHandle_t s_trigger_evt;
static char s_last_path[64];
static SemaphoreHandle_t s_state_lock;

static void wav_write_header(FILE *f, uint32_t sample_rate)
{
    /* Size fields stay 0; we patch them at close. */
    const uint8_t hdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0,
        1,0,                 /* PCM */
        1,0,                 /* 1 channel */
        0,0,0,0,             /* sample rate (patched) */
        0,0,0,0,             /* byte rate (patched) */
        2,0,                 /* block align */
        16,0,                /* bits per sample */
        'd','a','t','a', 0,0,0,0,
    };
    uint8_t buf[44];
    memcpy(buf, hdr, 44);
    uint32_t byte_rate = sample_rate * 2;
    memcpy(&buf[24], &sample_rate, 4);
    memcpy(&buf[28], &byte_rate, 4);
    fwrite(buf, 1, 44, f);
}

static void wav_patch_sizes(FILE *f, uint32_t data_bytes)
{
    uint32_t riff_size = 36 + data_bytes;
    fseek(f, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, f);
    fseek(f, 40, SEEK_SET);
    fwrite(&data_bytes, 4, 1, f);
    fflush(f);
}

static void make_filename(char *out, size_t max, const char *prefix, const char *subdir)
{
    time_t now = time(NULL);
    if (network_is_ntp_synced()) {
        struct tm tmv;
        gmtime_r(&now, &tmv);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", &tmv);
        snprintf(out, max, "%s/%s/cry-%s.wav", prefix, subdir, ts);
    } else {
        snprintf(out, max, "%s/%s/cry-boot%u.wav", prefix, subdir, (unsigned)(now & 0xffff));
    }
}

#define RETAIN_MAX 64
#define RETAIN_PATH_LEN 96

static void retain_newest_only(const char *dir, uint32_t keep)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    static char paths[RETAIN_MAX][RETAIN_PATH_LEN];
    size_t n = 0;
    while ((e = readdir(d)) && n < RETAIN_MAX) {
        if (strncmp(e->d_name, "cry-", 4) != 0) continue;
        int w = snprintf(paths[n], RETAIN_PATH_LEN, "%s/%.40s", dir, e->d_name);
        if (w > 0 && w < RETAIN_PATH_LEN) n++;
    }
    closedir(d);
    while (n > keep) {
        size_t oldest = 0;
        struct stat st_old, st_cur;
        stat(paths[0], &st_old);
        for (size_t i = 1; i < n; ++i) {
            stat(paths[i], &st_cur);
            if (st_cur.st_mtime < st_old.st_mtime) {
                oldest = i; st_old = st_cur;
            }
        }
        unlink(paths[oldest]);
        ESP_LOGI(TAG, "retained pruned: %s", paths[oldest]);
        for (size_t i = oldest; i + 1 < n; ++i) memcpy(paths[i], paths[i + 1], RETAIN_PATH_LEN);
        n--;
    }
}

static void preroll_push(const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        s_preroll[s_preroll_head] = pcm[i];
        s_preroll_head = (s_preroll_head + 1) % s_preroll_samples;
    }
    if (s_preroll_filled < s_preroll_samples) {
        s_preroll_filled += n;
        if (s_preroll_filled > s_preroll_samples) s_preroll_filled = s_preroll_samples;
    }
}

static void preroll_flush_to_file(FILE *f)
{
    size_t start = (s_preroll_head + s_preroll_samples - s_preroll_filled) % s_preroll_samples;
    size_t first_run = s_preroll_samples - start;
    if (first_run > s_preroll_filled) first_run = s_preroll_filled;
    fwrite(&s_preroll[start], sizeof(int16_t), first_run, f);
    if (first_run < s_preroll_filled) {
        fwrite(s_preroll, sizeof(int16_t), s_preroll_filled - first_run, f);
    }
}

static void recorder_task(void *arg)
{
    size_t chunk = 1600;
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "chunk alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t got = audio_capture_tap_read(s_tap, buf, chunk, portMAX_DELAY);
        if (got == 0) continue;

        if (!s_recording) {
            preroll_push(buf, got);
            if (xSemaphoreTake(s_trigger_evt, 0) == pdTRUE) {
                s_recording = true;
            } else {
                continue;
            }
        }

        /* When we transition to recording, open the file and flush pre-roll. */
        static FILE *f = NULL;
        static uint32_t recorded_samples = 0;
        static uint32_t postroll_target = 0;

        if (!f) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            make_filename(s_last_path, sizeof(s_last_path), s_cfg.mount_prefix, s_cfg.subdir);
            xSemaphoreGive(s_state_lock);
            f = fopen(s_last_path, "wb");
            if (!f) {
                ESP_LOGW(TAG, "fopen %s failed", s_last_path);
                s_recording = false;
                continue;
            }
            wav_write_header(f, s_cfg.sample_rate);
            preroll_flush_to_file(f);
            recorded_samples = (uint32_t)s_preroll_filled;
            postroll_target = recorded_samples + s_cfg.postroll_s * s_cfg.sample_rate;
            ESP_LOGI(TAG, "recording to %s (conf=%.2f, preroll=%u samples)",
                     s_last_path, (double)s_trigger_conf, (unsigned)s_preroll_filled);
        }

        fwrite(buf, sizeof(int16_t), got, f);
        recorded_samples += got;
        preroll_push(buf, got);

        if (recorded_samples >= postroll_target) {
            uint32_t data_bytes = recorded_samples * sizeof(int16_t);
            wav_patch_sizes(f, data_bytes);
            fclose(f); f = NULL;
            s_recording = false;
            ESP_LOGI(TAG, "record complete, bytes=%u", (unsigned)data_bytes);

            char dir[48];
            snprintf(dir, sizeof(dir), "%s/%s", s_cfg.mount_prefix, s_cfg.subdir);
            retain_newest_only(dir, s_cfg.keep_files);
        }
    }
}

esp_err_t event_recorder_init(const event_recorder_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_preroll_samples = cfg->preroll_s * cfg->sample_rate;
    s_preroll = heap_caps_calloc(s_preroll_samples, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_preroll) return ESP_ERR_NO_MEM;
    s_preroll_head = 0;
    s_preroll_filled = 0;

    snprintf(s_event_dir, sizeof(s_event_dir), "%s/%s", cfg->mount_prefix, cfg->subdir);
    mkdir(s_event_dir, 0777);

    s_tap = audio_capture_add_tap(48 * 1024);
    if (!s_tap) return ESP_FAIL;

    s_trigger_evt = xSemaphoreCreateBinary();
    s_state_lock  = xSemaphoreCreateMutex();
    if (!s_trigger_evt || !s_state_lock) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreatePinnedToCore(
        recorder_task, "rec", 4 * 1024, NULL, 6, &s_task, 1);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "init: preroll=%us postroll=%us at %s", (unsigned)cfg->preroll_s,
             (unsigned)cfg->postroll_s, s_event_dir);
    return ESP_OK;
}

const char *event_recorder_trigger(float cry_conf)
{
    if (s_recording) return NULL;
    s_trigger_conf = cry_conf;
    xSemaphoreGive(s_trigger_evt);
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    const char *p = s_last_path;
    xSemaphoreGive(s_state_lock);
    return p;
}

bool event_recorder_is_recording(void)
{
    return s_recording;
}

esp_err_t event_recorder_http_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    /* expect /events/<name> */
    const char *name = strrchr(uri, '/');
    if (!name) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "");
    name++;
    if (strstr(name, "..") || name[0] == '/' || name[0] == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "");
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", s_event_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "");

    httpd_resp_set_type(req, "audio/wav");
    char chunk[2048];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, got) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t event_recorder_list_handler(httpd_req_t *req)
{
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "[");
    DIR *d = opendir(s_event_dir);
    if (d) {
        struct dirent *e;
        bool first = true;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "cry-", 4) != 0) continue;
            n += snprintf(buf + n, sizeof(buf) - n,
                          "%s\"%s\"", first ? "" : ",", e->d_name);
            first = false;
            if (n >= (int)sizeof(buf) - 64) break;
        }
        closedir(d);
    }
    snprintf(buf + n, sizeof(buf) - n, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, strlen(buf));
}
