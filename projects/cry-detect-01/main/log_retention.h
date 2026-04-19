#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Periodic pruner for day-bucketed SD logs.
 *
 * Scans /sdcard once per CONFIG_CRY_LOG_RETENTION_PERIOD_S seconds and
 * unlinks any file matching `infer-YYYYMMDD.jsonl` or `cry-YYYYMMDD.log`
 * whose encoded date is older than CONFIG_CRY_LOG_RETENTION_DAYS days.
 *
 * WAV retention is NOT handled here — event_recorder.c keeps the
 * newest CRY_REC_KEEP_FILES WAVs on its own.
 *
 * Skips pruning while NTP is not synced (would mis-date "today").
 */
esp_err_t log_retention_init(void);

/* Monotonic count of files deleted since boot. */
uint32_t log_retention_total_deleted(void);

/* Files deleted in the last completed scan. */
uint32_t log_retention_last_deleted(void);
