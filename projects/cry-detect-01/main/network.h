#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*network_state_cb_t)(bool wifi_up, bool ntp_synced, void *ctx);

esp_err_t network_start(const char *mdns_hostname, network_state_cb_t cb, void *ctx);
bool network_is_wifi_up(void);
bool network_is_ntp_synced(void);
