#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "sync_ledger.h"   /* for sync_entry_t / SYNC_PATH_MAX etc. */

/* Pure helpers extracted from sync_ledger.c so they can be exercised
 * by host-compiled unit tests without dragging in FreeRTOS / heap_caps /
 * mbedtls / fopen.
 *
 * Two groups:
 *  - Table ops: pure operations on a (table, count) pair.
 *  - JSON walker: ad-hoc field extractor for the ledger row format.
 *
 * sync_ledger.c calls these against its module-private static table; the
 * test compiles them stand-alone with synthetic table arrays. */

/* ---- Table ops (pure data structure) ---- */

/* Find the index of `path` in the table, or -1 if not present. */
int sl_find_index(const sync_entry_t *table, int count, const char *path);

/* Insert or update entry. Returns the index of the row (>=0), or -1 if the
 * table is full and the path was new (no slot available). On update, the
 * existing entry is overwritten in place. */
int sl_upsert(sync_entry_t *table, int *count, int max,
              const sync_entry_t *e);

/* Remove the entry at index i by swap-from-tail. No bounds check beyond
 * the trivial guard: i out of range is a no-op. */
void sl_remove_index(sync_entry_t *table, int *count, int i);


/* ---- JSON walker (line-buffer field extractor) ---- */

/* Find the byte position right after the `key:` colon in the line, or
 * NULL if the key is not present. Whitespace after the colon is skipped.
 * Caller knows the expected value type. */
const char *sl_json_find_key(const char *line, const char *key);

/* Extract a quoted string value into `out`. Returns false on missing key,
 * non-string value, or buffer overflow. Does not de-escape — callers
 * (sync_ledger.c) round-trip 7-bit ASCII paths/hashes only. */
bool sl_json_extract_str(const char *line, const char *key,
                         char *out, size_t max);

/* Extract an unsigned integer value. Returns false on missing key. Uses
 * strtoul, so a non-numeric value returns 0 — caller cannot distinguish
 * "key absent" from "key present with value 0" via the return value, but
 * that's the existing behavior of the caller. */
bool sl_json_extract_uint(const char *line, const char *key, uint32_t *out);


/* ---- Hex encoding (used for sha256) ---- */

void sl_hex_encode(const uint8_t *raw, int n, char *out);
