// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_now.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t uptime_ms;
    uint8_t own_mac[ESP_NOW_ETH_ALEN];
    uint8_t peer_mac[ESP_NOW_ETH_ALEN];
    int channel;
    unsigned tx_dropped_packets;
    unsigned tx_dropped_bytes;
    unsigned rx_dropped_bytes;
    unsigned rx_received_packets;
    unsigned rx_received_bytes;
    unsigned rx_truncated_bytes;
    unsigned send_success_count;
    unsigned send_fail_count;
} bridge_status_t;

void bridge_get_status(bridge_status_t *out);

#if CONFIG_BRIDGE_DIAGNOSTICS_WEB
esp_err_t diagnostics_web_init_netif(void);
esp_err_t diagnostics_web_configure_softap(void);
esp_err_t diagnostics_web_start_http(void);
#endif

#ifdef __cplusplus
}
#endif
