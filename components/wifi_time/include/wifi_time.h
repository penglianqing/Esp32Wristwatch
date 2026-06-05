#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char * ssid;
    const char * password;
    const char * hostname;
    const char * static_ip;
    const char * gateway;
    const char * netmask;
    const char * dns;
    const char * timezone;
    const char * sntp_server;
    uint32_t connect_timeout_ms;
    uint32_t sync_timeout_ms;
} wifi_time_config_t;

esp_err_t wifi_time_start(const wifi_time_config_t * config);
bool wifi_time_is_connected(void);
bool wifi_time_is_synced(void);

#ifdef __cplusplus
}
#endif
