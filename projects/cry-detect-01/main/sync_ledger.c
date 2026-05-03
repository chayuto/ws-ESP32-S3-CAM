#include "sync_ledger.h"
#include "sync_ledger_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"

static const char *TAG = "sync_lg";

#if CONFIG_CRY_SYNC_LEDGER_ENABLED

static const char *LEDGER_PATH    = "/sdcard/.sync-ledger.jsonl";
static const char *LEDGER_TMP     = "/sdcard/.sync-ledger.tmp";
#define LEDGER_MAX_FILES          CONFIG_CRY_SYNC_LEDGER_MAX_FILES

/* Append-row threshold that triggers an automatic compaction. The on-disk
 * file can grow unboundedly (new register row per file + new ack row per
 * sync); compaction rewrites it to one row per path. Triggered inline at
 * append time when the count crosses this watermark. */
#define COMPACT_AT_APPEND_COUNT   2000

static SemaphoreHandle_t s_lock;
static sync_entry_t      *s_table;          /* size = LEDGER_MAX_FILES */
static int                s_count;          /* live entries in s_table */
static int                s_appends_since_compact;
static bool               s_initialized;

/* ---- helpers ----
 *
 * Pure operations on the table + JSON walker + hex_encode live in
 * sync_ledger_table.c so the host test can exercise them without
 * dragging in FreeRTOS / heap_caps / mbedtls. The thin wrappers below
 * keep the original call sites unchanged. */

static int find_index(const char *path)
{
    return sl_find_index(s_table, s_count, path);
}

static int upsert(const sync_entry_t *e)
{
    int idx = sl_upsert(s_table, &s_count, LEDGER_MAX_FILES, e);
    if (idx < 0) {
        ESP_LOGW(TAG, "table full at %d entries; dropping %s",
                 s_count, e->path);
    }
    return idx;
}

static void remove_index(int i)
{
    sl_remove_index(s_table, &s_count, i);
}

static esp_err_t sha256_of_file(const char *path, char *out_hex)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  /* 0 = SHA-256 (not -224) */

    const size_t buf_sz = 4096;
    uint8_t *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
    if (!buf) {
        mbedtls_sha256_free(&ctx);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got;
    while ((got = fread(buf, 1, buf_sz, f)) > 0) {
        mbedtls_sha256_update(&ctx, buf, got);
    }
    free(buf);
    fclose(f);

    uint8_t raw[32];
    mbedtls_sha256_finish(&ctx, raw);
    mbedtls_sha256_free(&ctx);
    sl_hex_encode(raw, 32, out_hex);
    return ESP_OK;
}

/* Append a single JSONL row. Always opens in "a" mode so concurrent fopen
 * across tasks is the only mutex needed (s_lock takes care of that). */
static esp_err_t append_row(const char *row, size_t row_len)
{
    FILE *f = fopen(LEDGER_PATH, "a");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(row, 1, row_len, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return (w == row_len) ? ESP_OK : ESP_FAIL;
}

static void format_register_row(const sync_entry_t *e, char *out, size_t max)
{
    snprintf(out, max,
        "{\"ts\":%ld,\"op\":\"register\",\"path\":\"%s\","
         "\"size\":%u,\"mtime\":%u,\"sha256\":\"%s\","
         "\"sync_state\":\"pending\",\"category\":\"%s\"}\n",
        (long)time(NULL), e->path, (unsigned)e->size, (unsigned)e->mtime,
        e->sha256_hex, e->category);
}

static void format_ack_row(const char *path, const char *sha, char *out, size_t max)
{
    snprintf(out, max,
        "{\"ts\":%ld,\"op\":\"ack\",\"path\":\"%s\",\"sha256\":\"%s\","
         "\"sync_state\":\"synced\"}\n",
        (long)time(NULL), path, sha ? sha : "");
}

static void format_purge_row(const char *path, char *out, size_t max)
{
    snprintf(out, max,
        "{\"ts\":%ld,\"op\":\"purge\",\"path\":\"%s\"}\n",
        (long)time(NULL), path);
}

/* ---- on-disk replay ---- */

static void replay_from_disk(void)
{
    FILE *f = fopen(LEDGER_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "no existing ledger at %s; starting empty", LEDGER_PATH);
        return;
    }
    char line[512];
    int rows = 0;
    while (fgets(line, sizeof(line), f)) {
        char op[16];
        if (!sl_json_extract_str(line, "op", op, sizeof(op))) continue;
        char path[SYNC_PATH_MAX];
        if (!sl_json_extract_str(line, "path", path, sizeof(path))) continue;

        if (strcmp(op, "purge") == 0) {
            int i = find_index(path);
            if (i >= 0) remove_index(i);
            rows++;
            continue;
        }

        if (strcmp(op, "register") == 0 || strcmp(op, "close") == 0) {
            sync_entry_t e;
            memset(&e, 0, sizeof(e));
            strncpy(e.path, path, SYNC_PATH_MAX - 1);
            (void)sl_json_extract_uint(line, "size", &e.size);
            (void)sl_json_extract_uint(line, "mtime", &e.mtime);
            (void)sl_json_extract_str(line, "sha256", e.sha256_hex, sizeof(e.sha256_hex));
            (void)sl_json_extract_str(line, "category", e.category, sizeof(e.category));
            char st[16] = "pending";
            (void)sl_json_extract_str(line, "sync_state", st, sizeof(st));
            e.state = (strcmp(st, "synced") == 0) ? SYNC_STATE_SYNCED : SYNC_STATE_PENDING;
            (void)upsert(&e);
            rows++;
            continue;
        }

        if (strcmp(op, "ack") == 0) {
            int i = find_index(path);
            if (i >= 0) {
                s_table[i].state = SYNC_STATE_SYNCED;
            }
            rows++;
            continue;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "replayed %d rows; live entries: %d", rows, s_count);
}

/* ---- compaction ---- */

esp_err_t sync_ledger_compact(void)
{
    if (!s_initialized) return ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    FILE *t = fopen(LEDGER_TMP, "w");
    if (!t) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    char row[400];
    for (int i = 0; i < s_count; ++i) {
        const sync_entry_t *e = &s_table[i];
        int n = snprintf(row, sizeof(row),
            "{\"ts\":%ld,\"op\":\"register\",\"path\":\"%s\","
             "\"size\":%u,\"mtime\":%u,\"sha256\":\"%s\","
             "\"sync_state\":\"%s\",\"category\":\"%s\"}\n",
            (long)time(NULL), e->path, (unsigned)e->size, (unsigned)e->mtime,
            e->sha256_hex,
            e->state == SYNC_STATE_SYNCED ? "synced" : "pending",
            e->category);
        if (n > 0) fwrite(row, 1, n, t);
    }
    /* Compaction marker for forensic readability. */
    int n = snprintf(row, sizeof(row),
        "{\"ts\":%ld,\"op\":\"compact\",\"entries\":%d}\n",
        (long)time(NULL), s_count);
    if (n > 0) fwrite(row, 1, n, t);
    fflush(t);
    fsync(fileno(t));
    fclose(t);

    /* Atomic-ish rename. ESP-IDF FATFS supports rename(). */
    int rc = rename(LEDGER_TMP, LEDGER_PATH);
    s_appends_since_compact = 0;
    xSemaphoreGive(s_lock);

    if (rc != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed (rc=%d)", LEDGER_TMP, LEDGER_PATH, rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "compacted: %d entries", s_count);
    return ESP_OK;
}

/* ---- public API ---- */

esp_err_t sync_ledger_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_table = heap_caps_calloc(LEDGER_MAX_FILES, sizeof(sync_entry_t),
                               MALLOC_CAP_SPIRAM);
    if (!s_table) {
        ESP_LOGE(TAG, "alloc %d entries failed", LEDGER_MAX_FILES);
        return ESP_ERR_NO_MEM;
    }
    s_count = 0;
    s_appends_since_compact = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    replay_from_disk();
    xSemaphoreGive(s_lock);

    s_initialized = true;
    ESP_LOGI(TAG, "init ok: capacity=%d, replayed=%d", LEDGER_MAX_FILES, s_count);
    return ESP_OK;
}

esp_err_t sync_ledger_register_closed(const char *path, const char *category)
{
    if (!s_initialized || !path) return ESP_OK;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_FAIL;

    sync_entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.path, path, SYNC_PATH_MAX - 1);
    e.size  = (uint32_t)st.st_size;
    e.mtime = (uint32_t)st.st_mtime;
    e.state = SYNC_STATE_PENDING;
    strncpy(e.category, category ? category : "file", SYNC_CATEGORY_MAX - 1);

    if (sha256_of_file(path, e.sha256_hex) != ESP_OK) {
        ESP_LOGW(TAG, "sha256 failed for %s", path);
        return ESP_FAIL;
    }

    char row[400];
    format_register_row(&e, row, sizeof(row));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = upsert(&e);
    esp_err_t rc = (i >= 0) ? append_row(row, strlen(row)) : ESP_FAIL;
    if (rc == ESP_OK) {
        s_appends_since_compact++;
    }
    bool need_compact = s_appends_since_compact > COMPACT_AT_APPEND_COUNT;
    xSemaphoreGive(s_lock);

    if (need_compact) {
        (void)sync_ledger_compact();
    }
    return rc;
}

esp_err_t sync_ledger_ack(const char *path, const char *expected_sha256_hex)
{
    if (!s_initialized || !path) return ESP_OK;
    char row[256];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_index(path);
    if (i < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_sha256_hex && expected_sha256_hex[0] &&
        strcmp(s_table[i].sha256_hex, expected_sha256_hex) != 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_table[i].state = SYNC_STATE_SYNCED;
    format_ack_row(path, s_table[i].sha256_hex, row, sizeof(row));
    s_appends_since_compact++;
    xSemaphoreGive(s_lock);

    return append_row(row, strlen(row));
}

bool sync_ledger_lookup(const char *path, sync_entry_t *out)
{
    if (!s_initialized || !path) return false;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_index(path);
    if (i >= 0) {
        if (out) *out = s_table[i];
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
}

esp_err_t sync_ledger_purge(const char *path)
{
    if (!s_initialized || !path) return ESP_OK;
    char row[160];
    format_purge_row(path, row, sizeof(row));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_index(path);
    if (i >= 0) remove_index(i);
    s_appends_since_compact++;
    xSemaphoreGive(s_lock);

    return append_row(row, strlen(row));
}

void sync_ledger_get_stats(sync_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_initialized) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->total_tracked = s_count;
    uint32_t oldest_pending = 0;
    for (int i = 0; i < s_count; ++i) {
        if (s_table[i].state == SYNC_STATE_SYNCED) {
            out->synced_count++;
            out->bytes_synced += s_table[i].size;
        } else {
            out->pending_count++;
            out->bytes_pending += s_table[i].size;
            if (oldest_pending == 0 || s_table[i].mtime < oldest_pending) {
                oldest_pending = s_table[i].mtime;
            }
        }
    }
    out->oldest_pending_mtime = oldest_pending;
    xSemaphoreGive(s_lock);
}

/* Iterator: yields entries with mtime > since_mtime, sorted ascending by
 * mtime (so pagination via next_since is monotonic). Linear-scan + insertion
 * sort into a small "best so far" array would be over-engineered; we copy
 * all matches into a small heap-allocated array and qsort. */

static int cmp_mtime_asc(const void *a, const void *b)
{
    const sync_entry_t *ea = (const sync_entry_t *)a;
    const sync_entry_t *eb = (const sync_entry_t *)b;
    if (ea->mtime < eb->mtime) return -1;
    if (ea->mtime > eb->mtime) return 1;
    return strcmp(ea->path, eb->path);
}

int sync_ledger_iterate(uint32_t since_mtime,
                        int limit,
                        sync_ledger_iter_cb_t cb,
                        void *ctx,
                        bool *truncated,
                        uint32_t *next_since)
{
    if (truncated) *truncated = false;
    if (next_since) *next_since = 0;
    if (!s_initialized || !cb) return 0;
    if (limit <= 0) limit = 500;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n_match = 0;
    for (int i = 0; i < s_count; ++i) {
        if (s_table[i].mtime > since_mtime) n_match++;
    }
    if (n_match == 0) {
        xSemaphoreGive(s_lock);
        return 0;
    }

    sync_entry_t *matched = heap_caps_malloc(n_match * sizeof(sync_entry_t),
                                             MALLOC_CAP_SPIRAM);
    if (!matched) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    int j = 0;
    for (int i = 0; i < s_count && j < n_match; ++i) {
        if (s_table[i].mtime > since_mtime) matched[j++] = s_table[i];
    }
    xSemaphoreGive(s_lock);

    qsort(matched, n_match, sizeof(sync_entry_t), cmp_mtime_asc);

    int yielded = 0;
    for (int i = 0; i < n_match && yielded < limit; ++i) {
        if (!cb(&matched[i], ctx)) break;
        yielded++;
    }
    if (truncated) *truncated = (yielded < n_match);
    if (next_since && yielded > 0) {
        *next_since = matched[yielded - 1].mtime;
    }
    free(matched);
    return yielded;
}

esp_err_t sync_ledger_reset(void)
{
    if (!s_initialized) return ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    s_appends_since_compact = 0;
    unlink(LEDGER_PATH);
    unlink(LEDGER_TMP);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "ledger reset");
    return ESP_OK;
}

#else  /* !CONFIG_CRY_SYNC_LEDGER_ENABLED — stub for unconditional callers */

esp_err_t sync_ledger_init(void)                                       { return ESP_OK; }
esp_err_t sync_ledger_register_closed(const char *p, const char *c)    { (void)p; (void)c; return ESP_OK; }
esp_err_t sync_ledger_ack(const char *p, const char *s)                { (void)p; (void)s; return ESP_OK; }
bool      sync_ledger_lookup(const char *p, sync_entry_t *o)           { (void)p; (void)o; return false; }
esp_err_t sync_ledger_purge(const char *p)                             { (void)p; return ESP_OK; }
void      sync_ledger_get_stats(sync_stats_t *o)                       { if (o) memset(o, 0, sizeof(*o)); }
int       sync_ledger_iterate(uint32_t s, int l, sync_ledger_iter_cb_t cb, void *c, bool *t, uint32_t *n)
                                                                       { (void)s; (void)l; (void)cb; (void)c; if (t) *t = false; if (n) *n = 0; return 0; }
esp_err_t sync_ledger_compact(void)                                    { return ESP_OK; }
esp_err_t sync_ledger_reset(void)                                      { return ESP_OK; }

#endif
