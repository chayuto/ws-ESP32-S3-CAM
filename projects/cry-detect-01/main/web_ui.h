#pragma once

#include "esp_err.h"
#include "metrics.h"

esp_err_t web_ui_start(uint32_t max_sse_clients);

/* Called by the detection pipeline to push a discrete event to SSE clients.
 * `type` is "detect" or "inference"; payload is an already-formatted JSON fragment. */
void web_ui_push_event(const char *type, const char *payload_json);
