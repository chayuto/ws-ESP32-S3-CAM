#include "sd_logger.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"

#include "bsp/esp-bsp.h"

#include "metrics.h"
#include "network.h"
#include "sdkconfig.h"

#if CONFIG_CRY_NOISE_FLOOR_ENABLED
#include "noise_floor.h"
#endif

static const char *TAG = "sdlog";

#define RING_LINES       80
/* v3 schema: ~280 chars typical. Raised from 448 to 640 after audit P1 #11
 * flagged the older limit as tight against truncation with 20 watched floats. */
#define RING_LINE_BYTES  640
#define RING_BYTES       (RING_LINES * RING_LINE_BYTES)

static char *s_ring;
static uint32_t s_ring_head;
static uint32_t s_ring_count;

static SemaphoreHandle_t s_lock;
static FILE *s_f;
static char s_path[64];
static uint32_t s_rotate_bytes;
static uint32_t s_written_in_file;
static uint32_t s_total_written;
static uint32_t s_last_flush_bytes;
static bool s_sd_mounted;
static bool s_fallback_fat_mounted;
static int s_seq_counter;

static const char *mount_prefix(void)
{
    return s_sd_mounted ? BSP_SD_MOUNT_POINT : "/logs";
}

static void make_path_locked(void)
{
    char tsdir[16];
    if (network_is_ntp_synced()) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(tsdir, sizeof(tsdir), "%Y%m%d", &tmv);
        snprintf(s_path, sizeof(s_path), "%s/cry-%s.log", mount_prefix(), tsdir);
    } else {
        snprintf(s_path, sizeof(s_path), "%s/cry-%04d.log", mount_prefix(), s_seq_counter++);
    }
}

static void reopen_locked(void)
{
    if (s_f) { fclose(s_f); s_f = NULL; }
    make_path_locked();
    s_f = fopen(s_path, "a");
    s_written_in_file = 0;
    s_last_flush_bytes = 0;
    if (!s_f) {
        metrics_increment_sd_write_error();
        ESP_LOGW(TAG, "fopen %s failed", s_path);
    } else {
        ESP_LOGI(TAG, "logging to %s", s_path);
    }
}

static esp_err_t mount_fallback_fat(void)
{
    const esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/logs", "logs_fat", &mcfg, &wl);
    if (err == ESP_OK) {
        s_fallback_fat_mounted = true;
        ESP_LOGI(TAG, "mounted fallback FAT at /logs");
    } else {
        ESP_LOGW(TAG, "fallback FAT mount failed: 0x%x", err);
    }
    return err;
}

esp_err_t sd_logger_init(const sd_logger_cfg_t *cfg)
{
    s_rotate_bytes = cfg->rotate_kb * 1024u;
    s_lock = xSemaphoreCreateMutex();

    s_ring = heap_caps_calloc(RING_BYTES, 1, MALLOC_CAP_INTERNAL);
    if (!s_ring) return ESP_ERR_NO_MEM;

    if (cfg->sd_enabled) {
        if (bsp_sdcard_mount() == ESP_OK) {
            s_sd_mounted = true;
            ESP_LOGI(TAG, "SD mounted at %s", BSP_SD_MOUNT_POINT);
        } else {
            ESP_LOGW(TAG, "SD not present; using internal fallback FAT");
            mount_fallback_fat();
        }
    } else {
        mount_fallback_fat();
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    reopen_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool sd_logger_is_sd_mounted(void)
{
    return s_sd_mounted;
}

const char *sd_logger_current_path(void)
{
    return s_f ? s_path : NULL;
}

static void ring_push(const char *line)
{
    size_t L = strnlen(line, RING_LINE_BYTES - 1);
    char *slot = &s_ring[(s_ring_head % RING_LINES) * RING_LINE_BYTES];
    memcpy(slot, line, L);
    slot[L] = 0;
    s_ring_head = (s_ring_head + 1) % RING_LINES;
    if (s_ring_count < RING_LINES) s_ring_count++;
}

static int format_timestamp(char *buf, size_t max)
{
    if (network_is_ntp_synced()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tmv;
        localtime_r(&tv.tv_sec, &tmv);
        int n = strftime(buf, max, "%Y-%m-%dT%H:%M:%S", &tmv);
        /* ISO-8601 local time with numeric offset (e.g. +11:00 AEDT / +10:00 AEST).
         * Output: 2026-04-18T08:15:42.123+11:00 */
        int m = snprintf(buf + n, max - n, ".%03ld", tv.tv_usec / 1000);
        n += m;
        m = strftime(buf + n, max - n, "%z", &tmv);  /* -> +1100 */
        /* insert ':' to make "+11:00" (proper RFC 3339) */
        if (m == 5 && (buf[n] == '+' || buf[n] == '-') && (size_t)(n + 7) < max) {
            memmove(buf + n + 4, buf + n + 3, 2);
            buf[n + 3] = ':';
            m++;
            buf[n + m] = '\0';  /* memmove clobbered the NUL */
        }
        return n + m;
    } else {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        return snprintf(buf, max, "up=%us,NOT_SYNCED", (unsigned)up);
    }
}

static void write_row_locked(const char *event, float cry_conf, int32_t latency_ms,
                             const cry_metrics_t *m, float nf_p95, bool nf_warm)
{
    char ts[48];
    format_timestamp(ts, sizeof(ts));
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);

    char line[RING_LINE_BYTES];
    /* schema (v2):
     * wallclock,uptime_s,event,cry_conf,max_cry_conf_1s,rms,nf_p95,nf_warm,
     * latency_ms,inference_count,inference_fps,free_heap,free_psram,rssi,state
     */
    int n = snprintf(line, sizeof(line),
        "%s,%u,%s,%.3f,%.3f,%.1f,%.1f,%d,%d,%u,%.2f,%u,%u,%d,%d",
        ts, (unsigned)up, event,
        (double)cry_conf,
        (double)m->max_cry_conf_1s,
        (double)m->input_rms,
        (double)nf_p95,
        (int)nf_warm,
        (int)latency_ms,
        (unsigned)m->inference_count,
        (double)m->inference_fps,
        (unsigned)m->free_heap,
        (unsigned)m->free_psram,
        (int)m->wifi_rssi,
        (int)m->state);
    if (n <= 0) return;

    /* v3 schema: 20 watched-class confidences, stable column order */
    for (int i = 0; i < CRY_WATCHED_N && n < (int)sizeof(line) - 8; ++i) {
        n += snprintf(line + n, sizeof(line) - n, ",%.3f",
                      (double)m->watched_conf[i]);
    }
    /* Detect truncation: snprintf returns the length it WOULD have written.
     * If that exceeds our buffer, widen RING_LINE_BYTES or drop columns. */
    if (n >= (int)sizeof(line) - 2) {
        metrics_increment_sd_write_error();
        ESP_LOGW(TAG, "row truncated (n=%d, cap=%d) — widen RING_LINE_BYTES",
                 n, (int)sizeof(line));
        n = (int)sizeof(line) - 2;
    }
    line[n++] = '\n';
    line[n]   = '\0';

    ring_push(line);
    if (s_f) {
        size_t w = fwrite(line, 1, n, s_f);
        if (w != (size_t)n) {
            /* Short write = SD full or I/O error. Surface to metrics,
             * close the handle so next write reopens + retries (P1 #14). */
            metrics_increment_sd_write_error();
            ESP_LOGW(TAG, "fwrite short (%u/%d) on %s", (unsigned)w, n, s_path);
            fclose(s_f); s_f = NULL;
            return;
        }
        s_written_in_file += w;
        s_total_written += w;
        if (s_written_in_file >= s_rotate_bytes) {
            reopen_locked();
        } else if (s_written_in_file - s_last_flush_bytes >= 2048) {
            /* fflush alone leaves data in FATFS's cache; fsync pushes to SD.
             * Without fsync, a power loss / reboot loses everything since
             * the last fclose (which is every rotation or at NTP sync). */
            if (fflush(s_f) != 0 || fsync(fileno(s_f)) != 0) {
                metrics_increment_sd_write_error();
                ESP_LOGW(TAG, "fflush/fsync failed on %s", s_path);
            }
            s_last_flush_bytes = s_written_in_file;
        }
    }
}

void sd_logger_event(const char *event, float cry_conf, int32_t latency_ms)
{
    if (!s_lock) return;                 /* pre-init call (hygiene audit P0 #2) */
    cry_metrics_t m;
    metrics_snapshot(&m);
    float nf_p95 = 0.0f;
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    nf_p95 = noise_floor_p95();
#endif

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool nf_warm = false;
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    nf_warm = noise_floor_is_warm();
#endif
    write_row_locked(event, cry_conf, latency_ms, &m, nf_p95, nf_warm);
    xSemaphoreGive(s_lock);
}

void sd_logger_snapshot(void)
{
    if (!s_lock) return;                 /* pre-init call (hygiene audit P0 #2) */
    cry_metrics_t m;
    metrics_snapshot(&m);
    float nf_p95 = 0.0f;
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    nf_p95 = noise_floor_p95();
#endif

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool nf_warm = false;
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    nf_warm = noise_floor_is_warm();
#endif
    write_row_locked("snapshot", m.last_cry_conf, m.last_inference_ms, &m, nf_p95, nf_warm);
    xSemaphoreGive(s_lock);
}

void sd_logger_ntp_sync_marker(void)
{
    if (!s_lock) return;                 /* pre-init call (hygiene audit P0 #2) */
    cry_metrics_t m;
    metrics_snapshot(&m);
    float nf_p95 = 0.0f;
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    nf_p95 = noise_floor_p95();
#endif

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Force a new file so the post-NTP entries are in a wallclock-stamped file
     * with a clear boundary marker. */
    bool nf_warm = false;
#if CONFIG_CRY_NOISE_FLOOR_ENABLED
    nf_warm = noise_floor_is_warm();
#endif
    write_row_locked("ntp_synced", 0.0f, 0, &m, nf_p95, nf_warm);
    reopen_locked();
    write_row_locked("ntp_file_begin", 0.0f, 0, &m, nf_p95, nf_warm);
    xSemaphoreGive(s_lock);
}

size_t sd_logger_tail(char *dst, size_t dst_max, uint32_t lines)
{
    if (!s_lock) return 0;               /* pre-init call (hygiene audit P0 #2) */
    if (lines > RING_LINES) lines = RING_LINES;
    if (lines > s_ring_count) lines = s_ring_count;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t start = (s_ring_head + RING_LINES - lines) % RING_LINES;
    size_t written = 0;
    for (uint32_t i = 0; i < lines; ++i) {
        const char *src = &s_ring[((start + i) % RING_LINES) * RING_LINE_BYTES];
        size_t L = strnlen(src, RING_LINE_BYTES);
        if (written + L + 1 > dst_max) break;
        memcpy(dst + written, src, L);
        written += L;
    }
    xSemaphoreGive(s_lock);
    return written;
}
