#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* All handlers below enforce the whitelist of allowed roots (see path_safe
 * in file_api.c) and reject any query containing ".." or bare backslash.
 * Handlers stream large payloads via httpd_resp_send_chunk and allocate
 * big buffers on the PSRAM heap so the HTTP task stack stays small.
 *
 * Register on the shared httpd handle alongside the rest of web_ui's URIs. */

esp_err_t file_api_ls(httpd_req_t *req);     /* GET  /files/ls?path=... */
esp_err_t file_api_get(httpd_req_t *req);    /* GET  /files/get?path=...[&range=start-end] */
esp_err_t file_api_head(httpd_req_t *req);   /* GET  /files/head?path=...&bytes=N */
esp_err_t file_api_tail(httpd_req_t *req);   /* GET  /files/tail?path=...&bytes=N */
esp_err_t file_api_stat(httpd_req_t *req);   /* GET  /files/stat?path=...[&hash=1] */
esp_err_t file_api_rm(httpd_req_t *req);     /* DELETE /files/rm?path=... (via GET method OK too) */
esp_err_t file_api_df(httpd_req_t *req);     /* GET  /files/df */
