#include "breadcrumb.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG  = "bc";
static const char *NS   = "cry_det";      /* shared with event_recorder boot counter */
static const char *KLAST= "bc/last";      /* current-boot breadcrumb */
static const char *KPREV= "bc/prev";      /* previous-boot breadcrumb (read once at init) */
static const char *KBOOT= "boot_n";       /* monotonic boot counter */

#define BC_MAX 192

static char s_now[BC_MAX]  = "{\"stage\":\"pre-init\",\"up\":0}";
static char s_prev[BC_MAX] = "";
static uint32_t s_boot = 0;

void breadcrumb_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed — breadcrumb disabled");
        return;
    }

    /* Load previous-boot last breadcrumb (the one that was live when we
     * rebooted). Keep a copy in RAM for status_json. */
    size_t len = sizeof(s_prev);
    esp_err_t rc = nvs_get_str(h, KLAST, s_prev, &len);
    if (rc == ESP_OK && len > 1) {
        ESP_LOGI(TAG, "prev-boot breadcrumb: %s", s_prev);
        /* Copy to prev slot for later retrieval; leave last slot alone
         * (it will be overwritten by the first breadcrumb_set). */
        (void)nvs_set_str(h, KPREV, s_prev);
    } else {
        s_prev[0] = '\0';
        ESP_LOGI(TAG, "no previous breadcrumb");
    }

    /* Advance boot counter. */
    (void)nvs_get_u32(h, KBOOT, &s_boot);
    s_boot++;
    (void)nvs_set_u32(h, KBOOT, s_boot);
    (void)nvs_commit(h);
    nvs_close(h);

    breadcrumb_set("boot");
}

void breadcrumb_set(const char *stage)
{
    if (!stage) return;
    time_t now = 0;
    time(&now);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);

    int n = snprintf(s_now, sizeof(s_now),
        "{\"stage\":\"%.32s\",\"up\":%u,\"epoch\":%lld,\"boot\":%u}",
        stage, (unsigned)up, (long long)now, (unsigned)s_boot);
    if (n <= 0 || n >= (int)sizeof(s_now)) return;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_str(h, KLAST, s_now);
    (void)nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "%s", s_now);
}

size_t breadcrumb_status_json(char *buf, size_t max)
{
    int n = snprintf(buf, max,
        "{\"now\":%s,\"prev_boot\":%s,\"boot_counter\":%u}",
        s_now[0]  ? s_now  : "null",
        s_prev[0] ? s_prev : "null",
        (unsigned)s_boot);
    return (size_t)(n > 0 ? n : 0);
}

uint32_t breadcrumb_boot_counter(void) { return s_boot; }
