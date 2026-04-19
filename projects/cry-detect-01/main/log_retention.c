#include "log_retention.h"

#include "sdkconfig.h"

#if CONFIG_CRY_LOG_RETENTION_ENABLED

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "network.h"

static const char *TAG = "logret";

#define SCAN_DIR          "/sdcard"
#define DAYS_KEEP         CONFIG_CRY_LOG_RETENTION_DAYS
#define PERIOD_MS         (CONFIG_CRY_LOG_RETENTION_PERIOD_S * 1000)
#define FIRST_SCAN_DELAY  60000  /* let NTP + SD settle before first prune */

static uint32_t s_last_deleted;
static uint32_t s_total_deleted;

/* Parse 8 digits after the first '-' as YYYYMMDD and return "days since
 * the UNIX epoch midnight" (not a real epoch day, but a monotonically
 * increasing ordinal — all we need for age comparison).
 *
 * Returns -1 on any parse failure. Accepts both `infer-YYYYMMDD.jsonl`
 * and `cry-YYYYMMDD.log`. */
static int filename_day_ordinal(const char *name)
{
    const char *dash = strchr(name, '-');
    if (!dash) return -1;
    const char *p = dash + 1;
    for (int i = 0; i < 8; ++i) {
        if (p[i] < '0' || p[i] > '9') return -1;
    }
    int y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    int mo = (p[4]-'0')*10 + (p[5]-'0');
    int d  = (p[6]-'0')*10 + (p[7]-'0');
    if (y < 2020 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) return -1;

    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon  = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 12;  /* noon avoids DST edge cases */
    time_t t = mktime(&tmv);
    if (t < 0) return -1;
    return (int)(t / 86400);
}

/* Return true if `name` matches one of the day-bucketed log patterns we
 * own. Strict length+suffix check so we don't eat anything unexpected. */
static bool is_daybucketed_log(const char *name)
{
    size_t n = strlen(name);
    /* "infer-YYYYMMDD.jsonl" → 20 chars */
    if (n == 20 && strncmp(name, "infer-", 6) == 0 &&
        strcmp(name + 14, ".jsonl") == 0) return true;
    /* "cry-YYYYMMDD.log" → 16 chars */
    if (n == 16 && strncmp(name, "cry-", 4) == 0 &&
        strcmp(name + 12, ".log") == 0) return true;
    return false;
}

static void scan_and_prune(void)
{
    s_last_deleted = 0;

    if (!network_is_ntp_synced()) {
        /* Can't tell what "today" is without NTP. Skip rather than
         * delete by boot-local clock and risk wiping real captures. */
        ESP_LOGD(TAG, "skip scan: NTP not synced");
        return;
    }

    time_t now = time(NULL);
    int today_ordinal = (int)(now / 86400);

    DIR *d = opendir(SCAN_DIR);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s failed", SCAN_DIR);
        return;
    }

    char path[280];
    struct dirent *e;
    uint32_t deleted = 0;
    uint32_t scanned = 0;

    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) continue;
        if (!is_daybucketed_log(e->d_name)) continue;
        scanned++;
        int ord = filename_day_ordinal(e->d_name);
        if (ord < 0) continue;
        int age = today_ordinal - ord;
        if (age <= DAYS_KEEP) continue;

        snprintf(path, sizeof(path), "%s/%s", SCAN_DIR, e->d_name);
        if (unlink(path) == 0) {
            ESP_LOGI(TAG, "deleted %s (age=%d d, keep=%d)",
                     e->d_name, age, DAYS_KEEP);
            deleted++;
        } else {
            ESP_LOGW(TAG, "unlink %s failed", path);
        }
    }
    closedir(d);

    s_last_deleted = deleted;
    s_total_deleted += deleted;
    ESP_LOGI(TAG, "scan: %u day-logs scanned, %u deleted (total=%u)",
             (unsigned)scanned, (unsigned)deleted, (unsigned)s_total_deleted);
}

static void retention_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "retention task: period=%ds keep=%dd",
             CONFIG_CRY_LOG_RETENTION_PERIOD_S, DAYS_KEEP);

    vTaskDelay(pdMS_TO_TICKS(FIRST_SCAN_DELAY));

    for (;;) {
        scan_and_prune();
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}

esp_err_t log_retention_init(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        retention_task, "logret", 4 * 1024, NULL, 1, NULL, 0);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

uint32_t log_retention_total_deleted(void) { return s_total_deleted; }
uint32_t log_retention_last_deleted(void)  { return s_last_deleted; }

#else  /* !CONFIG_CRY_LOG_RETENTION_ENABLED */

esp_err_t log_retention_init(void) { return ESP_OK; }
uint32_t log_retention_total_deleted(void) { return 0; }
uint32_t log_retention_last_deleted(void)  { return 0; }

#endif
