#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Phase B HTTP endpoints. Register on the shared httpd_handle_t in web_ui.c. */

esp_err_t sync_api_manifest(httpd_req_t *req);  /* GET  /manifest.json */
esp_err_t sync_api_ack(httpd_req_t *req);       /* POST /sync/ack */
