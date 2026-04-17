#include "network.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "protocol_examples_common.h"

static const char *TAG = "net";

static network_state_cb_t s_cb;
static void *s_ctx;
static bool s_wifi_up;
static bool s_ntp_synced;

static void on_time_sync(struct timeval *tv)
{
    s_ntp_synced = true;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    ESP_LOGI(TAG, "time synced: %s", ts);
    if (s_cb) s_cb(s_wifi_up, s_ntp_synced, s_ctx);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_up = true;
        ESP_LOGI(TAG, "wifi up");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        sntp_set_time_sync_notification_cb(on_time_sync);
        esp_sntp_init();
        /* Australia/Sydney — POSIX TZ with AEST/AEDT rules. No tzdata file
         * needed on the device; the POSIX string is self-describing. */
        setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
        tzset();
        if (s_cb) s_cb(s_wifi_up, s_ntp_synced, s_ctx);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        if (s_cb) s_cb(s_wifi_up, s_ntp_synced, s_ctx);
    }
}

static esp_err_t start_mdns(const char *hostname)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    mdns_hostname_set(hostname);
    mdns_instance_name_set("cry-detect-01");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_instance_name_set("_http", "_tcp", "cry-detect-01 web UI");
    ESP_LOGI(TAG, "mDNS: %s.local / _http._tcp", hostname);
    return ESP_OK;
}

esp_err_t network_start(const char *mdns_hostname, network_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,      on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_ip_event, NULL));

    ESP_ERROR_CHECK(example_connect());
    return start_mdns(mdns_hostname);
}

bool network_is_wifi_up(void)
{
    return s_wifi_up;
}

bool network_is_ntp_synced(void)
{
    return s_ntp_synced;
}
