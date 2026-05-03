/* Host-test stub for ESP-IDF's esp_err.h.
 * We only need the type alias for sync_ledger.h to parse. */
#pragma once
typedef int esp_err_t;
#define ESP_OK                 0
#define ESP_FAIL              -1
#define ESP_ERR_NO_MEM        -2
#define ESP_ERR_NOT_FOUND     -3
#define ESP_ERR_INVALID_STATE -4
