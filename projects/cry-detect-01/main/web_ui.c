#include "web_ui.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "sdkconfig.h"

#include "sd_logger.h"

#if CONFIG_CRY_STREAM_COMPILED_IN
#include "audio_stream.h"
#endif
#if CONFIG_CRY_REC_COMPILED_IN
#include "event_recorder.h"
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
    char buf[768];
    size_t n = metrics_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handler_healthz(httpd_req_t *req)
{
    return httpd_resp_send(req, "OK", 2);
}

static esp_err_t handler_log_tail(httpd_req_t *req)
{
    char buf[8192];
    size_t n = sd_logger_tail(buf, sizeof(buf), 50);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, n);
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
    char buf[768];
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
    cfg.max_uri_handlers = 12;
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
    httpd_register_uri_handler(s_server, &h_index);
    httpd_register_uri_handler(s_server, &h_js);
    httpd_register_uri_handler(s_server, &h_metrics);
    httpd_register_uri_handler(s_server, &h_events);
    httpd_register_uri_handler(s_server, &h_log);
    httpd_register_uri_handler(s_server, &h_health);

#if CONFIG_CRY_STREAM_COMPILED_IN
    httpd_uri_t h_stream = { .uri = "/audio.pcm", .method = HTTP_GET, .handler = audio_stream_http_handler };
    httpd_register_uri_handler(s_server, &h_stream);
#endif
#if CONFIG_CRY_REC_COMPILED_IN
    httpd_uri_t h_rec_list = { .uri = "/events/list", .method = HTTP_GET, .handler = event_recorder_list_handler };
    httpd_uri_t h_rec_file = { .uri = "/recordings/*", .method = HTTP_GET, .handler = event_recorder_http_handler };
    httpd_register_uri_handler(s_server, &h_rec_list);
    httpd_register_uri_handler(s_server, &h_rec_file);
#endif

    ESP_LOGI(TAG, "http server up on :80 (up to %u SSE clients)", (unsigned)s_max_clients);
    return ESP_OK;
}
