#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool sd_enabled;
    uint32_t rotate_kb;
} sd_logger_cfg_t;

esp_err_t sd_logger_init(const sd_logger_cfg_t *cfg);
bool sd_logger_is_sd_mounted(void);

/* Returns the path to the currently open log file, or NULL if not open.
 * Stable pointer into internal storage; do not free. Not thread-safe
 * against concurrent reopen_locked, but good enough for UI/debug use. */
const char *sd_logger_current_path(void);

/* Single-line CSV event. Columns (stable):
 *   wallclock,uptime_s,event,cry_conf,input_rms,noise_p95,latency_ms,free_heap,rssi,state
 * wallclock is ISO-8601 UTC if NTP synced else "NOT_SYNCED".
 * uptime_s is monotonic and always present — use it to cross-reference
 * pre-NTP rows with post-NTP rows. Thread-safe. */
void sd_logger_event(const char *event, float cry_conf, int32_t latency_ms);

/* Log a comprehensive metrics snapshot. Same CSV schema as sd_logger_event
 * with event="snapshot", called periodically from housekeeping. */
void sd_logger_snapshot(void);

/* Emit a marker line when NTP transitions to synced, so downstream readers
 * can map uptime_s-stamped rows to wall clock retroactively. */
void sd_logger_ntp_sync_marker(void);

/* Copy last N lines from the in-RAM ring buffer to dst. Returns bytes written. */
size_t sd_logger_tail(char *dst, size_t dst_max, uint32_t lines);
