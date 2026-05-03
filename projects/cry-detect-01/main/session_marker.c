#include "session_marker.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs.h"

#include "breadcrumb.h"
#include "network.h"
#include "sd_logger.h"
#include "sdkconfig.h"

#if CONFIG_CRY_SYNC_LEDGER_ENABLED
#include "sync_ledger.h"
#endif

static const char *TAG = "session";
static const char *NS_NAME       = "cry_det";       /* shared NVS namespace */
static const char *KMARKER_BOOT  = "sync_marker_n"; /* boot_count of last marker */
static const char *KGEN_ID       = "sync_gen_id";   /* generation_id (string) */

void session_marker_maybe_write(void)
{
#if CONFIG_CRY_SYNC_SESSION_MARKER
    static bool s_done = false;
    if (s_done) return;

    if (!sd_logger_is_sd_mounted()) return;
    if (!network_is_ntp_synced())  return;

    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) return;

    uint32_t marker_boot = 0;
    (void)nvs_get_u32(h, KMARKER_BOOT, &marker_boot);
    uint32_t cur_boot = breadcrumb_boot_counter();
    if (marker_boot == cur_boot && marker_boot != 0) {
        /* Already wrote a marker for this boot. */
        nvs_close(h);
        s_done = true;
        return;
    }

    /* Build the ISO8601 timestamp twice: one for the filename
     * (compact, +TZ) and one for the body (RFC-3339 with colon in TZ). */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char iso_compact[40];
    strftime(iso_compact, sizeof(iso_compact), "%Y%m%dT%H%M%S%z", &tmv);

    char iso_pretty[40];
    int np = strftime(iso_pretty, sizeof(iso_pretty), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    if (np == 24 && (iso_pretty[19] == '+' || iso_pretty[19] == '-')) {
        memmove(iso_pretty + 23, iso_pretty + 22, 2);
        iso_pretty[22] = ':';
        iso_pretty[25] = '\0';
    }

    /* Read or initialize generation_id. First-ever boot of this firmware
     * mints one; thereafter sticky in NVS until manually erased. */
    char gen_id[40] = "";
    size_t gid_len = sizeof(gen_id);
    if (nvs_get_str(h, KGEN_ID, gen_id, &gid_len) != ESP_OK) {
        strftime(gen_id, sizeof(gen_id), "%Y%m%dT%H%M%SZ", &tmv);
        (void)nvs_set_str(h, KGEN_ID, gen_id);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const char *fw_version = app ? app->version : "unknown";

    char path[80];
    snprintf(path, sizeof(path), "/sdcard/.session-started-%s.json", iso_compact);

    char body[320];
    int n = snprintf(body, sizeof(body),
        "{\"firmware_version\":\"%s\","
         "\"boot_count\":%u,"
         "\"generation_id\":\"%s\","
         "\"mounted_at\":\"%s\"}\n",
        fw_version, (unsigned)cur_boot, gen_id, iso_pretty);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "failed to open %s for write", path);
        nvs_close(h);
        return;
    }
    size_t w = fwrite(body, 1, n, f);
    fflush(f);
    fclose(f);

    if (w != (size_t)n) {
        ESP_LOGW(TAG, "short write on %s (%u/%d)", path, (unsigned)w, n);
        nvs_close(h);
        return;
    }

    /* Persist that we wrote it for this boot. Subsequent calls are no-ops. */
    (void)nvs_set_u32(h, KMARKER_BOOT, cur_boot);
    (void)nvs_commit(h);
    nvs_close(h);

#if CONFIG_CRY_SYNC_LEDGER_ENABLED
    /* The marker is a closed/immutable file; register so it shows in /manifest.json. */
    (void)sync_ledger_register_closed(path, "session_marker");
#endif

    ESP_LOGI(TAG, "session marker: %s", path);
    s_done = true;
#endif /* CONFIG_CRY_SYNC_SESSION_MARKER */
}
