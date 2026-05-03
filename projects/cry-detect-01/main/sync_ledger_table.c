#include "sync_ledger_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Table ops ---- */

int sl_find_index(const sync_entry_t *table, int count, const char *path)
{
    if (!table || !path) return -1;
    for (int i = 0; i < count; ++i) {
        if (strcmp(table[i].path, path) == 0) return i;
    }
    return -1;
}

int sl_upsert(sync_entry_t *table, int *count, int max,
              const sync_entry_t *e)
{
    if (!table || !count || !e) return -1;
    int i = sl_find_index(table, *count, e->path);
    if (i >= 0) {
        table[i] = *e;
        return i;
    }
    if (*count >= max) return -1;
    table[*count] = *e;
    int idx = *count;
    (*count)++;
    return idx;
}

void sl_remove_index(sync_entry_t *table, int *count, int i)
{
    if (!table || !count) return;
    if (i < 0 || i >= *count) return;
    if (i != *count - 1) {
        table[i] = table[*count - 1];
    }
    (*count)--;
}

/* ---- JSON walker ---- */

const char *sl_json_find_key(const char *line, const char *key)
{
    if (!line || !key) return NULL;
    char needle[24];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return NULL;
    const char *p = strstr(line, needle);
    if (!p) return NULL;
    p += n;
    while (*p == ' ') p++;
    return p;
}

bool sl_json_extract_str(const char *line, const char *key,
                         char *out, size_t max)
{
    if (!out || max == 0) return false;
    const char *p = sl_json_find_key(line, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < max) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return *p == '"';
}

bool sl_json_extract_uint(const char *line, const char *key, uint32_t *out)
{
    if (!out) return false;
    const char *p = sl_json_find_key(line, key);
    if (!p) return false;
    *out = (uint32_t)strtoul(p, NULL, 10);
    return true;
}

/* ---- Hex encoding ---- */

void sl_hex_encode(const uint8_t *raw, int n, char *out)
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < n; ++i) {
        out[2 * i]     = H[(raw[i] >> 4) & 0xF];
        out[2 * i + 1] = H[raw[i] & 0xF];
    }
    out[2 * n] = '\0';
}
