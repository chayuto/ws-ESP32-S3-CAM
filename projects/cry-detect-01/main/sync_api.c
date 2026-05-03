#include "sync_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "sync_ledger.h"

static const char *TAG = "sync_api";

#if CONFIG_CRY_SYNC_API_ENABLED

/* ---- helpers ---- */

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

static int read_query_int(httpd_req_t *req, const char *key, int defval)
{
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) return defval;
    char vbuf[24];
    if (httpd_query_key_value(qbuf, key, vbuf, sizeof(vbuf)) != ESP_OK) return defval;
    return atoi(vbuf);
}

static void load_generation_id(char *out, size_t max)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("cry_det", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = max;
    (void)nvs_get_str(h, "sync_gen_id", out, &len);
    nvs_close(h);
    if (out[0] == '\0') {
        snprintf(out, max, "unknown");
    }
}

/* Iterator context for streaming manifest rows. */
typedef struct {
    httpd_req_t *req;
    bool first;
    int  emitted;
} manifest_ctx_t;

static bool emit_entry(const sync_entry_t *e, void *vctx)
{
    manifest_ctx_t *c = (manifest_ctx_t *)vctx;
    char row[480];
    int n = snprintf(row, sizeof(row),
        "%s{\"path\":\"%s\",\"size\":%u,\"mtime\":%u,"
         "\"sha256\":\"%s\",\"sync_state\":\"%s\","
         "\"category\":\"%s\",\"mutable\":false}",
        c->first ? "" : ",",
        e->path, (unsigned)e->size, (unsigned)e->mtime,
        e->sha256_hex,
        e->state == SYNC_STATE_SYNCED ? "synced" : "pending",
        e->category);
    if (n <= 0) return true;
    if (httpd_resp_send_chunk(c->req, row, n) != ESP_OK) return false;
    c->first = false;
    c->emitted++;
    return true;
}

/* ---- /manifest.json ---- */

esp_err_t sync_api_manifest(httpd_req_t *req)
{
    int since = read_query_int(req, "since", 0);
    int limit = read_query_int(req, "limit", 500);
    if (limit <= 0) limit = 500;
    if (limit > 2000) limit = 2000;

    char gen_id[40];
    load_generation_id(gen_id, sizeof(gen_id));
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    long now_unix = (long)time(NULL);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char head[256];
    int hn = snprintf(head, sizeof(head),
        "{\"generation_id\":\"%s\",\"device_uptime_s\":%u,\"now\":%ld,\"files\":[",
        gen_id, (unsigned)up_s, now_unix);
    if (httpd_resp_send_chunk(req, head, hn) != ESP_OK) return ESP_FAIL;

    manifest_ctx_t ctx = { .req = req, .first = true, .emitted = 0 };
    bool truncated = false;
    uint32_t next_since = 0;
    int yielded = sync_ledger_iterate((uint32_t)since, limit, emit_entry, &ctx,
                                      &truncated, &next_since);
    (void)yielded;

    char tail[160];
    int tn;
    if (truncated && next_since > 0) {
        tn = snprintf(tail, sizeof(tail), "],\"truncated\":true,\"next_since\":%u}", (unsigned)next_since);
    } else {
        tn = snprintf(tail, sizeof(tail), "],\"truncated\":false,\"next_since\":null}");
    }
    httpd_resp_send_chunk(req, tail, tn);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- /sync/ack ----
 * Body: {"files":[{"path":"...","sha256":"..."}, ...]}
 * We don't pull in cJSON; lightweight string-walking parser is sufficient
 * for our schema (no nesting beyond one array of flat objects, no
 * embedded escape sequences in paths/hashes that we generate ourselves). */

static const char *skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

/* Extract a JSON-string value into `out`, advancing *cursor past the closing
 * quote. Returns false on malformed input or buffer overflow. Does NOT
 * de-escape — paths/hashes we round-trip are 7-bit ASCII without
 * backslashes by construction. */
static bool extract_str(const char **cursor, char *out, size_t max)
{
    const char *p = skip_ws(*cursor);
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (i + 1 >= max) return false;
        out[i++] = *p++;
    }
    if (*p != '"') return false;
    out[i] = '\0';
    *cursor = p + 1;
    return true;
}

esp_err_t sync_api_ack(httpd_req_t *req)
{
    /* Bound the body. 4 KB is plenty for a few hundred path+sha entries. */
    int total = req->content_len;
    if (total <= 0 || total > 8192) {
        return fail(req, "400 Bad Request", "body too small or too large");
    }
    char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
    if (!body) return fail(req, "503 Service Unavailable", "no heap");

    int read = 0;
    while (read < total) {
        int r = httpd_req_recv(req, body + read, total - read);
        if (r <= 0) {
            free(body);
            return fail(req, "400 Bad Request", "recv failed");
        }
        read += r;
    }
    body[total] = '\0';

    /* Walk to "files":[ ... ] */
    const char *p = strstr(body, "\"files\"");
    if (!p) { free(body); return fail(req, "400 Bad Request", "missing files"); }
    p += strlen("\"files\"");
    p = skip_ws(p);
    if (*p++ != ':') { free(body); return fail(req, "400 Bad Request", "expected :"); }
    p = skip_ws(p);
    if (*p++ != '[') { free(body); return fail(req, "400 Bad Request", "expected ["); }

    int acked = 0;
    int rejected = 0;
    /* Streamed reject list — capped to a reasonable buffer for the response. */
    char rejected_paths[512];
    rejected_paths[0] = '\0';

    while (1) {
        p = skip_ws(p);
        if (*p == ']') { p++; break; }
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        p++;
        char path[SYNC_PATH_MAX] = "";
        char sha[80] = "";
        while (1) {
            p = skip_ws(p);
            if (*p == '}') { p++; break; }
            if (*p == ',') { p++; continue; }
            char key[16];
            if (!extract_str(&p, key, sizeof(key))) break;
            p = skip_ws(p);
            if (*p++ != ':') break;
            char val[160];
            if (!extract_str(&p, val, sizeof(val))) break;
            if (strcmp(key, "path") == 0) {
                strncpy(path, val, sizeof(path) - 1);
            } else if (strcmp(key, "sha256") == 0) {
                strncpy(sha, val, sizeof(sha) - 1);
            }
        }
        if (path[0]) {
            esp_err_t ar = sync_ledger_ack(path, sha[0] ? sha : NULL);
            if (ar == ESP_OK) {
                acked++;
            } else {
                rejected++;
                size_t cur = strlen(rejected_paths);
                size_t want = strlen(path) + 4;
                if (cur + want < sizeof(rejected_paths)) {
                    snprintf(rejected_paths + cur, sizeof(rejected_paths) - cur,
                             "%s\"%s\"", cur ? "," : "", path);
                }
                ESP_LOGW(TAG, "ack rejected: %s rc=0x%x", path, ar);
            }
        }
    }
    free(body);

    char resp[640];
    int n = snprintf(resp, sizeof(resp),
        "{\"acked\":%d,\"rejected\":%d,\"rejected_paths\":[%s]}",
        acked, rejected, rejected_paths);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, n);
}

#else  /* !CONFIG_CRY_SYNC_API_ENABLED — stub returns 404 */

esp_err_t sync_api_manifest(httpd_req_t *req)
{
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "sync API disabled", HTTPD_RESP_USE_STRLEN);
}
esp_err_t sync_api_ack(httpd_req_t *req)
{
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "sync API disabled", HTTPD_RESP_USE_STRLEN);
}

#endif
