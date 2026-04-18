#include "file_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "ff.h"
#include "diskio.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "esp_partition.h"

#include "sd_logger.h"

static const char *TAG = "fileapi";

/* ---- whitelist + path safety ---- */

static bool path_safe(const char *p)
{
    if (!p || p[0] != '/') return false;
    if (strstr(p, "..") || strchr(p, '\\')) return false;
    static const char *const ROOTS[] = {
        "/sdcard", "/logs", "/yamnet", NULL
    };
    for (int i = 0; ROOTS[i]; ++i) {
        size_t rl = strlen(ROOTS[i]);
        if (strncmp(p, ROOTS[i], rl) == 0 && (p[rl] == '\0' || p[rl] == '/')) {
            return true;
        }
    }
    return false;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

static bool read_query_path(httpd_req_t *req, char *out, size_t max)
{
    char qbuf[256];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) return false;
    if (httpd_query_key_value(qbuf, "path", out, max) != ESP_OK) return false;
    return true;
}

static int read_query_int(httpd_req_t *req, const char *key, int defval)
{
    char qbuf[256];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) return defval;
    char vbuf[16];
    if (httpd_query_key_value(qbuf, key, vbuf, sizeof(vbuf)) != ESP_OK) return defval;
    return atoi(vbuf);
}

static const char *content_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".log")) return "text/plain";
    if (!strcasecmp(dot, ".csv")) return "text/csv";
    if (!strcasecmp(dot, ".txt")) return "text/plain";
    if (!strcasecmp(dot, ".wav")) return "audio/wav";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".tflite")) return "application/octet-stream";
    return "application/octet-stream";
}

/* ---- /files/ls ---- */

esp_err_t file_api_ls(httpd_req_t *req)
{
    char path[160];
    if (!read_query_path(req, path, sizeof(path))) return fail(req, "400 Bad Request", "missing path");
    if (!path_safe(path)) return fail(req, "400 Bad Request", "path not in whitelist");

    DIR *d = opendir(path);
    if (!d) return fail(req, "404 Not Found", "opendir failed");

    httpd_resp_set_type(req, "application/json");
    char head[128];
    int n = snprintf(head, sizeof(head), "{\"path\":\"%s\",\"entries\":[", path);
    httpd_resp_send_chunk(req, head, n);

    struct dirent *e;
    bool first = true;
    char entry[320];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[320];
        snprintf(full, sizeof(full), "%s/%.128s", path, e->d_name);
        struct stat st = {0};
        stat(full, &st);
        bool is_dir = S_ISDIR(st.st_mode);
        n = snprintf(entry, sizeof(entry),
                     "%s{\"name\":\"%.128s\",\"type\":\"%s\",\"size\":%ld,\"mtime\":%ld}",
                     first ? "" : ",",
                     e->d_name,
                     is_dir ? "dir" : "file",
                     (long)st.st_size,
                     (long)st.st_mtime);
        httpd_resp_send_chunk(req, entry, n);
        first = false;
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- /files/get ---- */

esp_err_t file_api_get(httpd_req_t *req)
{
    char path[160];
    if (!read_query_path(req, path, sizeof(path))) return fail(req, "400 Bad Request", "missing path");
    if (!path_safe(path)) return fail(req, "400 Bad Request", "path not in whitelist");

    FILE *f = fopen(path, "rb");
    if (!f) return fail(req, "404 Not Found", "fopen failed");

    httpd_resp_set_type(req, content_type_for(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    size_t chunk_sz = 4096;
    char *buf = heap_caps_malloc(chunk_sz, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return fail(req, "503 Service Unavailable", "no heap"); }

    size_t got;
    esp_err_t rc = ESP_OK;
    while ((got = fread(buf, 1, chunk_sz, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, got) != ESP_OK) { rc = ESP_FAIL; break; }
    }
    free(buf);
    fclose(f);
    if (rc == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return rc;
}

/* ---- /files/head + /files/tail ---- */

static esp_err_t head_or_tail(httpd_req_t *req, bool tail)
{
    char path[160];
    if (!read_query_path(req, path, sizeof(path))) return fail(req, "400 Bad Request", "missing path");
    if (!path_safe(path)) return fail(req, "400 Bad Request", "path not in whitelist");
    int bytes = read_query_int(req, "bytes", 4096);
    if (bytes < 1) bytes = 1;
    if (bytes > 1024 * 1024) bytes = 1024 * 1024;

    FILE *f = fopen(path, "rb");
    if (!f) return fail(req, "404 Not Found", "fopen failed");
    if (tail) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        long off = sz > bytes ? sz - bytes : 0;
        fseek(f, off, SEEK_SET);
    }
    httpd_resp_set_type(req, content_type_for(path));
    char *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return fail(req, "503 Service Unavailable", "no heap"); }
    size_t got = fread(buf, 1, bytes, f);
    fclose(f);
    esp_err_t rc = httpd_resp_send(req, buf, got);
    free(buf);
    return rc;
}

esp_err_t file_api_head(httpd_req_t *req) { return head_or_tail(req, false); }
esp_err_t file_api_tail(httpd_req_t *req) { return head_or_tail(req, true);  }

/* ---- /files/stat ---- */

esp_err_t file_api_stat(httpd_req_t *req)
{
    char path[160];
    if (!read_query_path(req, path, sizeof(path))) return fail(req, "400 Bad Request", "missing path");
    if (!path_safe(path)) return fail(req, "400 Bad Request", "path not in whitelist");

    struct stat st;
    if (stat(path, &st) != 0) return fail(req, "404 Not Found", "stat failed");

    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"path\":\"%s\",\"size\":%ld,\"mtime\":%ld,\"is_dir\":%s}",
                     path, (long)st.st_size, (long)st.st_mtime,
                     S_ISDIR(st.st_mode) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/* ---- /files/rm ---- */

esp_err_t file_api_rm(httpd_req_t *req)
{
    char path[160];
    if (!read_query_path(req, path, sizeof(path))) return fail(req, "400 Bad Request", "missing path");
    if (!path_safe(path)) return fail(req, "400 Bad Request", "path not in whitelist");

    struct stat st;
    if (stat(path, &st) != 0) return fail(req, "404 Not Found", "stat failed");
    if (S_ISDIR(st.st_mode)) return fail(req, "400 Bad Request", "is a directory; no rmdir");

    const char *cur = sd_logger_current_path();
    if (cur && strcmp(cur, path) == 0) {
        return fail(req, "409 Conflict", "cannot delete currently open log file");
    }

    if (unlink(path) != 0) return fail(req, "500 Internal Server Error", "unlink failed");
    char body[128];
    int n = snprintf(body, sizeof(body), "{\"deleted\":\"%s\"}", path);
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "rm %s", path);
    return httpd_resp_send(req, body, n);
}

/* ---- /files/df ----
 * No statvfs in ESP-IDF newlib. Hand-query each filesystem's native API. */

static void fat_df(httpd_req_t *req, const char *name, const char *mount,
                   const char *fatfs_drv, bool first)
{
    uint64_t total_b = 0, free_b = 0;
    (void)esp_vfs_fat_info(mount, &total_b, &free_b);
    (void)fatfs_drv;
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "%s\"%s\":{\"mount\":\"%s\",\"fs\":\"fat\","
                     "\"total_bytes\":%llu,\"free_bytes\":%llu}",
                     first ? "" : ",", name, mount,
                     (unsigned long long)total_b, (unsigned long long)free_b);
    httpd_resp_send_chunk(req, buf, n);
}

static void spiffs_df(httpd_req_t *req, const char *name, const char *label,
                      const char *mount, bool first)
{
    size_t total = 0, used = 0;
    (void)esp_spiffs_info(label, &total, &used);
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "%s\"%s\":{\"mount\":\"%s\",\"fs\":\"spiffs\","
                     "\"total_bytes\":%u,\"used_bytes\":%u,\"free_bytes\":%u}",
                     first ? "" : ",", name, mount,
                     (unsigned)total, (unsigned)used, (unsigned)(total - used));
    httpd_resp_send_chunk(req, buf, n);
}

esp_err_t file_api_df(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{", 1);
    fat_df(req,    "sdcard", "/sdcard", "",   true);
    fat_df(req,    "logs",   "/logs",   "",   false);
    spiffs_df(req, "yamnet", "yamnet",  "/yamnet", false);
    httpd_resp_send_chunk(req, "}", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- /files/coredump* ---- */

static const esp_partition_t *coredump_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                    NULL);
}

esp_err_t file_api_coredump_info(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    esp_err_t rc = esp_core_dump_image_get(&addr, &size);
    bool present = (rc == ESP_OK && size > 0);

    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"present\":%s,\"size\":%u,\"addr\":%u,\"check\":\"0x%x\"}",
        present ? "true" : "false",
        (unsigned)size, (unsigned)addr, (unsigned)rc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

esp_err_t file_api_coredump_get(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        return fail(req, "404 Not Found", "no coredump present");
    }
    const esp_partition_t *p = coredump_partition();
    if (!p) return fail(req, "500 Internal Server Error", "no coredump partition");
    if (addr < p->address || addr + size > p->address + p->size) {
        return fail(req, "500 Internal Server Error", "coredump address out of partition");
    }
    size_t offset = addr - p->address;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"cry-detect-01-coredump.elf\"");

    size_t chunk_sz = 4096;
    uint8_t *buf = heap_caps_malloc(chunk_sz, MALLOC_CAP_SPIRAM);
    if (!buf) return fail(req, "503 Service Unavailable", "no heap");

    esp_err_t rc = ESP_OK;
    size_t remaining = size;
    while (remaining > 0 && rc == ESP_OK) {
        size_t to_read = remaining > chunk_sz ? chunk_sz : remaining;
        if (esp_partition_read(p, offset, buf, to_read) != ESP_OK) { rc = ESP_FAIL; break; }
        if (httpd_resp_send_chunk(req, (char *)buf, to_read) != ESP_OK) { rc = ESP_FAIL; break; }
        offset += to_read;
        remaining -= to_read;
    }
    free(buf);
    if (rc == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "coredump stream: size=%u rc=0x%x", (unsigned)size, rc);
    return rc;
}

esp_err_t file_api_coredump_erase(httpd_req_t *req)
{
    esp_err_t rc = esp_core_dump_image_erase();
    char body[64];
    int n = snprintf(body, sizeof(body), "{\"erased\":%s,\"rc\":\"0x%x\"}",
                     rc == ESP_OK ? "true" : "false", (unsigned)rc);
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "coredump erase rc=0x%x", rc);
    return httpd_resp_send(req, body, n);
}
