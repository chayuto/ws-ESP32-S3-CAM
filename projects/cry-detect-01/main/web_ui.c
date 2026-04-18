#include "web_ui.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "sdkconfig.h"

#include "sd_logger.h"
#include "file_api.h"
#include "led_alert.h"
#include "breadcrumb.h"

#if CONFIG_CRY_STREAM_COMPILED_IN
#include "audio_stream.h"
#endif
#if CONFIG_CRY_REC_COMPILED_IN
#include "event_recorder.h"
#include <dirent.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#endif

static const char *TAG = "web";

#define MAX_CLIENTS 4

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");

typedef struct {
    int sockfd;
    bool in_use;
} sse_client_t;

static httpd_handle_t s_server;
static sse_client_t s_clients[MAX_CLIENTS];
static uint32_t s_max_clients;
static SemaphoreHandle_t s_lock;

static void send_raw(int fd, const char *s, size_t n)
{
    ssize_t sent = 0;
    while (sent < (ssize_t)n) {
        ssize_t r = send(fd, s + sent, n - sent, 0);
        if (r < 0) return;
        sent += r;
    }
}

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t handler_app_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, app_js_start, app_js_end - app_js_start);
}

static esp_err_t handler_metrics(httpd_req_t *req)
{
    char buf[2048];
    size_t n = metrics_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handler_healthz(httpd_req_t *req)
{
    return httpd_resp_send(req, "OK", 2);
}

/* /status — breadcrumb + boot counter + previous-boot state (P2 #23).
 * If the previous boot crashed silently (brown-out, WDT, assert before
 * coredump init), `prev_boot.stage` tells you *where* it died. */
static esp_err_t handler_status(httpd_req_t *req)
{
    char buf[512];
    size_t n = breadcrumb_status_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* P2 #22: runtime log-level control. GET /log/level?tag=X&level=debug
 * Levels: none, error, warn, info, debug, verbose.  tag "*" hits everything. */
static esp_err_t handler_log_level(httpd_req_t *req)
{
    char qbuf[128];
    char tag[32] = "*";
    char lvl[16] = "info";
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "tag", tag, sizeof(tag));
        httpd_query_key_value(qbuf, "level", lvl, sizeof(lvl));
    }
    esp_log_level_t target = ESP_LOG_INFO;
    if      (!strcasecmp(lvl, "none"))    target = ESP_LOG_NONE;
    else if (!strcasecmp(lvl, "error"))   target = ESP_LOG_ERROR;
    else if (!strcasecmp(lvl, "warn"))    target = ESP_LOG_WARN;
    else if (!strcasecmp(lvl, "info"))    target = ESP_LOG_INFO;
    else if (!strcasecmp(lvl, "debug"))   target = ESP_LOG_DEBUG;
    else if (!strcasecmp(lvl, "verbose")) target = ESP_LOG_VERBOSE;
    esp_log_level_set(tag, target);
    char resp[96];
    int n = snprintf(resp, sizeof(resp),
                     "{\"tag\":\"%s\",\"level\":\"%s\"}", tag, lvl);
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "log_level tag=%s level=%s", tag, lvl);
    return httpd_resp_send(req, resp, n);
}

#if CONFIG_CRY_REC_COMPILED_IN
/* Manual-trigger for training-data collection. POST /record/trigger[?note=...]
 * The detector threshold is unreliable right now (INT8 compression), so this
 * lets an operator mark real cry events as they happen. */
static esp_err_t handler_record_trigger(httpd_req_t *req)
{
    char qbuf[160];
    char note[96] = "";
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "note", note, sizeof(note));
    }
    bool ok = event_recorder_trigger_manual(note);
    char resp[160];
    int n = snprintf(resp, sizeof(resp),
        "{\"triggered\":%s,\"recording\":%s,\"note\":\"%.64s\"}",
        ok ? "true" : "false",
        event_recorder_is_recording() ? "true" : "false",
        note);
    httpd_resp_set_type(req, "application/json");
    if (!ok) httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_send(req, resp, n);
}

/* GET /record/status — capacity estimate + recording state.
 * Scans the events dir for WAV count and total bytes, compares against
 * SD free space. Useful for deciding how long you can leave a collection
 * session running before SD fills. */
static esp_err_t handler_record_status(httpd_req_t *req)
{
    const char *dir = event_recorder_dir();
    size_t wav_count = 0, wav_bytes = 0;
    size_t label_bytes = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char full[160];
        struct stat st;
        while ((e = readdir(d)) != NULL) {
            snprintf(full, sizeof(full), "%s/%.80s", dir, e->d_name);
            if (stat(full, &st) != 0) continue;
            if (!S_ISREG(st.st_mode)) continue;
            if (strstr(e->d_name, ".wav")) { wav_count++; wav_bytes += st.st_size; }
            else if (!strcmp(e->d_name, "triggers.jsonl")) label_bytes = st.st_size;
        }
        closedir(d);
    }
    uint64_t total_b = 0, free_b = 0;
    (void)esp_vfs_fat_info("/sdcard", &total_b, &free_b);
    /* WAV size = sample_rate * 2 bytes * (preroll+postroll) seconds.
     * Hard-code the compile-time math for the status response. */
    size_t wav_size_est = 16000 * 2 * (CONFIG_CRY_REC_PREROLL_S + CONFIG_CRY_REC_POSTROLL_S) + 44;
    size_t estimated_capacity = wav_size_est > 0 ? free_b / wav_size_est : 0;

    char buf[384];
    int n = snprintf(buf, sizeof(buf),
        "{\"dir\":\"%s\",\"recording\":%s,"
        "\"wav_count\":%u,\"wav_bytes\":%u,\"label_bytes\":%u,"
        "\"sd_free_bytes\":%llu,\"wav_size_bytes_est\":%u,"
        "\"capacity_estimate_wavs\":%u}",
        dir,
        event_recorder_is_recording() ? "true" : "false",
        (unsigned)wav_count, (unsigned)wav_bytes, (unsigned)label_bytes,
        (unsigned long long)free_b, (unsigned)wav_size_est,
        (unsigned)estimated_capacity);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}
#endif /* CONFIG_CRY_REC_COMPILED_IN */

static esp_err_t handler_led_brightness(httpd_req_t *req)
{
    char qbuf[64];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(qbuf, "pct", val, sizeof(val)) == ESP_OK) {
            int pct = atoi(val);
            led_alert_set_brightness(pct);
        }
    }
    char resp[64];
    int n = snprintf(resp, sizeof(resp),
                     "{\"brightness\":%d}", led_alert_get_brightness());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, n);
}

static esp_err_t handler_log_tail(httpd_req_t *req)
{
    /* 8 KB off the handler task's 6 KB stack would corrupt it — use PSRAM heap. */
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "no heap", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    size_t n = sd_logger_tail(buf, 8192, 50);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t rc = httpd_resp_send(req, buf, n);
    free(buf);
    return rc;
}

static int register_client(int fd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = -1;
    for (uint32_t i = 0; i < s_max_clients; ++i) {
        if (!s_clients[i].in_use && slot < 0) slot = i;
    }
    if (slot >= 0) {
        s_clients[slot].sockfd = fd;
        s_clients[slot].in_use = true;
    }
    xSemaphoreGive(s_lock);
    return slot;
}

static void drop_client(int slot)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (slot >= 0 && slot < (int)s_max_clients) {
        s_clients[slot].in_use = false;
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t handler_events(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    int slot = register_client(fd);
    if (slot < 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "too many SSE clients", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n\r\n";
    send_raw(fd, hdr, strlen(hdr));

    /* Initial snapshot to let the page render. */
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "event: snapshot\ndata: ");
    n += metrics_to_json(buf + n, sizeof(buf) - n);
    n += snprintf(buf + n, sizeof(buf) - n, "\n\n");
    send_raw(fd, buf, n);

    /* Keep the task alive while the browser is connected. esp_http_server
     * will detach this request handler's thread when it returns; we rely on
     * the detected state from push_event via the registered socket. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        const char *hb = "event: heartbeat\ndata: {}\n\n";
        ssize_t r = send(fd, hb, strlen(hb), 0);
        if (r <= 0) {
            drop_client(slot);
            return ESP_OK;
        }
    }
}

void web_ui_push_event(const char *type, const char *payload_json)
{
    if (!s_lock) return;                 /* web UI not started yet */
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "event: %s\ndata: %s\n\n", type, payload_json);
    if (n <= 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0; i < s_max_clients; ++i) {
        if (s_clients[i].in_use) {
            ssize_t r = send(s_clients[i].sockfd, buf, n, MSG_DONTWAIT);
            if (r < 0) s_clients[i].in_use = false;
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t web_ui_start(uint32_t max_sse_clients)
{
    s_max_clients = max_sse_clients > MAX_CLIENTS ? MAX_CLIENTS : max_sse_clients;
    s_lock = xSemaphoreCreateMutex();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 32;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t h_index   = { .uri = "/",        .method = HTTP_GET, .handler = handler_index };
    httpd_uri_t h_js      = { .uri = "/app.js",  .method = HTTP_GET, .handler = handler_app_js };
    httpd_uri_t h_metrics = { .uri = "/metrics", .method = HTTP_GET, .handler = handler_metrics };
    httpd_uri_t h_events  = { .uri = "/events",  .method = HTTP_GET, .handler = handler_events };
    httpd_uri_t h_log     = { .uri = "/log/tail",.method = HTTP_GET, .handler = handler_log_tail };
    httpd_uri_t h_health  = { .uri = "/healthz", .method = HTTP_GET, .handler = handler_healthz };
    httpd_uri_t h_led     = { .uri = "/led/brightness", .method = HTTP_GET, .handler = handler_led_brightness };
    httpd_uri_t h_loglvl  = { .uri = "/log/level",      .method = HTTP_GET, .handler = handler_log_level };
    httpd_uri_t h_status  = { .uri = "/status",         .method = HTTP_GET, .handler = handler_status };
    httpd_register_uri_handler(s_server, &h_index);
    httpd_register_uri_handler(s_server, &h_js);
    httpd_register_uri_handler(s_server, &h_metrics);
    httpd_register_uri_handler(s_server, &h_events);
    httpd_register_uri_handler(s_server, &h_log);
    httpd_register_uri_handler(s_server, &h_health);
    httpd_register_uri_handler(s_server, &h_led);
    httpd_register_uri_handler(s_server, &h_loglvl);
    httpd_register_uri_handler(s_server, &h_status);

    httpd_uri_t h_ls   = { .uri = "/files/ls",   .method = HTTP_GET,    .handler = file_api_ls   };
    httpd_uri_t h_get  = { .uri = "/files/get",  .method = HTTP_GET,    .handler = file_api_get  };
    httpd_uri_t h_head = { .uri = "/files/head", .method = HTTP_GET,    .handler = file_api_head };
    httpd_uri_t h_tail = { .uri = "/files/tail", .method = HTTP_GET,    .handler = file_api_tail };
    httpd_uri_t h_stat = { .uri = "/files/stat", .method = HTTP_GET,    .handler = file_api_stat };
    httpd_uri_t h_df   = { .uri = "/files/df",   .method = HTTP_GET,    .handler = file_api_df   };
    httpd_uri_t h_rm   = { .uri = "/files/rm",   .method = HTTP_DELETE, .handler = file_api_rm   };
    httpd_uri_t h_cdi  = { .uri = "/files/coredump/info", .method = HTTP_GET,    .handler = file_api_coredump_info };
    httpd_uri_t h_cdg  = { .uri = "/files/coredump",      .method = HTTP_GET,    .handler = file_api_coredump_get  };
    httpd_uri_t h_cde  = { .uri = "/files/coredump",      .method = HTTP_DELETE, .handler = file_api_coredump_erase };
    httpd_register_uri_handler(s_server, &h_ls);
    httpd_register_uri_handler(s_server, &h_get);
    httpd_register_uri_handler(s_server, &h_head);
    httpd_register_uri_handler(s_server, &h_tail);
    httpd_register_uri_handler(s_server, &h_stat);
    httpd_register_uri_handler(s_server, &h_df);
    httpd_register_uri_handler(s_server, &h_rm);
    httpd_register_uri_handler(s_server, &h_cdi);
    httpd_register_uri_handler(s_server, &h_cdg);
    httpd_register_uri_handler(s_server, &h_cde);

#if CONFIG_CRY_STREAM_COMPILED_IN
    httpd_uri_t h_stream = { .uri = "/audio.pcm", .method = HTTP_GET, .handler = audio_stream_http_handler };
    httpd_register_uri_handler(s_server, &h_stream);
#endif
#if CONFIG_CRY_REC_COMPILED_IN
    httpd_uri_t h_rec_list = { .uri = "/events/list", .method = HTTP_GET, .handler = event_recorder_list_handler };
    httpd_uri_t h_rec_file = { .uri = "/recordings/*", .method = HTTP_GET, .handler = event_recorder_http_handler };
    httpd_uri_t h_rec_trig  = { .uri = "/record/trigger", .method = HTTP_POST, .handler = handler_record_trigger };
    /* GET alias so a phone browser address bar or a bookmarked link works
     * without a form/fetch. Same handler, same query-string parsing. */
    httpd_uri_t h_rec_trigg = { .uri = "/record/trigger", .method = HTTP_GET,  .handler = handler_record_trigger };
    httpd_uri_t h_rec_stat  = { .uri = "/record/status",  .method = HTTP_GET,  .handler = handler_record_status };
    httpd_register_uri_handler(s_server, &h_rec_list);
    httpd_register_uri_handler(s_server, &h_rec_file);
    httpd_register_uri_handler(s_server, &h_rec_trig);
    httpd_register_uri_handler(s_server, &h_rec_trigg);
    httpd_register_uri_handler(s_server, &h_rec_stat);
#endif

    ESP_LOGI(TAG, "http server up on :80 (up to %u SSE clients)", (unsigned)s_max_clients);
    return ESP_OK;
}
