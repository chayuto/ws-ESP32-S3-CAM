#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    uint32_t preroll_s;
    uint32_t postroll_s;
    uint32_t keep_files;
    uint32_t sample_rate;
    const char *mount_prefix;    /* "/sdcard" or "/logs" */
    const char *subdir;          /* "events" */
} event_recorder_cfg_t;

esp_err_t event_recorder_init(const event_recorder_cfg_t *cfg);

/* Kick off a new post-trigger recording that flushes the pre-roll first.
 * Returns the relative file path (stable pointer to internal storage) for
 * the web UI log. Returns NULL if a recording is already in flight. */
const char *event_recorder_trigger(float cry_conf);

/* Serve files under /recordings. */
esp_err_t event_recorder_http_handler(httpd_req_t *req);

/* Enumerate existing WAVs (JSON). */
esp_err_t event_recorder_list_handler(httpd_req_t *req);

bool event_recorder_is_recording(void);
