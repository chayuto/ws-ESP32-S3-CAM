#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool sd_enabled;
    uint32_t rotate_kb;
} sd_logger_cfg_t;

esp_err_t sd_logger_init(const sd_logger_cfg_t *cfg);
bool sd_logger_is_sd_mounted(void);

/* Format and append a CSV line. The timestamp is chosen automatically:
 * ISO-8601 if NTP is synced, uptime-seconds otherwise. Thread-safe. */
void sd_logger_event(const char *event, float cry_conf, int32_t latency_ms);

/* Copy last N lines from the in-RAM ring buffer to dst. Returns bytes written. */
size_t sd_logger_tail(char *dst, size_t dst_max, uint32_t lines);
