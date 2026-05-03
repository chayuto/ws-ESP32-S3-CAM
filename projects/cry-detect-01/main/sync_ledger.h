#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "sdkconfig.h"

/* Phase B sync ledger.
 *
 * On-disk:  /sdcard/.sync-ledger.jsonl  (append-only JSONL, hourly compaction)
 * In-memory: fixed-size table of sync_entry_t, linear-scanned (path counts
 *            are in the hundreds-to-low-thousands; strcmp scans are cheap
 *            relative to the SD I/O on registration / manifest building).
 *
 * Concurrency: all public functions take an internal mutex. Safe to call
 * from logger/inference/HTTPD tasks.
 *
 * Lifecycle: sync_ledger_init() at boot loads the existing on-disk ledger
 * (if any) and rebuilds the in-memory state. Returns ESP_OK on a fresh
 * card with no ledger yet. Calls before init are no-ops.
 *
 * When CONFIG_CRY_SYNC_LEDGER_ENABLED is off, all public calls return
 * ESP_OK and the iterator yields nothing. Callers don't need #ifdef. */

#define SYNC_PATH_MAX        80
#define SYNC_CATEGORY_MAX    16
#define SYNC_SHA256_HEX_LEN  64

typedef enum {
    SYNC_STATE_PENDING = 0,
    SYNC_STATE_SYNCED  = 1,
} sync_state_t;

typedef struct {
    char        path[SYNC_PATH_MAX];
    uint32_t    size;
    uint32_t    mtime;
    char        sha256_hex[SYNC_SHA256_HEX_LEN + 1];
    sync_state_t state;
    char        category[SYNC_CATEGORY_MAX];
} sync_entry_t;

typedef struct {
    uint32_t pending_count;
    uint32_t synced_count;
    uint64_t bytes_pending;
    uint64_t bytes_synced;
    uint32_t oldest_pending_mtime;   /* 0 if none pending */
    uint32_t total_tracked;
} sync_stats_t;

/* Initialize the ledger module. Loads /sdcard/.sync-ledger.jsonl if present
 * and replays into the in-memory table. */
esp_err_t sync_ledger_init(void);

/* Register a closed/immutable file. Computes its sha256 by reading the file
 * and writes a new register row to the ledger. State starts as PENDING.
 * If `category` is NULL, defaults to "file". Returns ESP_OK on success. */
esp_err_t sync_ledger_register_closed(const char *path, const char *category);

/* Mark `path` as synced. expected_sha256_hex must match the current ledger
 * entry's sha; on mismatch returns ESP_ERR_INVALID_STATE without changing
 * state. NULL expected_sha256_hex is allowed and skips the check
 * (only for trusted local callers). */
esp_err_t sync_ledger_ack(const char *path, const char *expected_sha256_hex);

/* Look up an entry by path. Returns false if not present. Fills `out` on
 * success. */
bool sync_ledger_lookup(const char *path, sync_entry_t *out);

/* Mark a path as deleted (purge row + remove from in-memory table). */
esp_err_t sync_ledger_purge(const char *path);

/* Stats snapshot for /metrics. */
void sync_ledger_get_stats(sync_stats_t *out);

/* Iterator callback for /manifest.json streaming. Return false to stop. */
typedef bool (*sync_ledger_iter_cb_t)(const sync_entry_t *e, void *ctx);

/* Iterate entries with mtime > since_mtime, up to `limit` rows. Returns the
 * number of entries yielded. If iteration was capped (more entries
 * available), `*truncated` is set true and `*next_since` to the last yielded
 * mtime. */
int sync_ledger_iterate(uint32_t since_mtime,
                        int limit,
                        sync_ledger_iter_cb_t cb,
                        void *ctx,
                        bool *truncated,
                        uint32_t *next_since);

/* Force a compaction now (rewrite ledger to canonical form). Called by
 * a periodic task; exposed for tests / admin. */
esp_err_t sync_ledger_compact(void);

/* Reset (wipe both in-memory + on-disk). Used after manual SD wipe so the
 * device starts with an empty ledger. NOT exposed via HTTP. */
esp_err_t sync_ledger_reset(void);
