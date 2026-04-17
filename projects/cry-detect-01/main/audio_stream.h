#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t audio_stream_init(uint32_t max_listeners, uint32_t ring_kb);

/* Registered on the web UI's httpd handle — serves chunked L16 PCM. */
esp_err_t audio_stream_http_handler(httpd_req_t *req);

uint32_t audio_stream_listener_count(void);
bool audio_stream_is_active(void);
