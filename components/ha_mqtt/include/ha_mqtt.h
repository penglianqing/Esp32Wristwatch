#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char * uri;
    const char * username;
    const char * password;
    const char * client_id;
    const char * fnos_cpu_topic;
    const char * fnos_mem_topic;
    const char * fnos_gpu_topic;
    const char * fnos_tx_topic;
    const char * fnos_rx_topic;
    const char * win_cpu_topic;
    const char * win_mem_topic;
    const char * win_gpu_topic;
    const char * win_tx_topic;
    const char * win_rx_topic;
    const char * weather_topic;
} ha_mqtt_config_t;

esp_err_t ha_mqtt_start(const ha_mqtt_config_t * config);

#ifdef __cplusplus
}
#endif
