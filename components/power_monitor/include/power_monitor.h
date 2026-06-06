#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*power_monitor_battery_cb_t)(int32_t percent, void * user_ctx);
typedef void (*power_monitor_power_button_cb_t)(void * user_ctx);

esp_err_t power_monitor_start(power_monitor_battery_cb_t battery_cb, void * user_ctx);
int32_t power_monitor_get_battery_percent(void);
void power_monitor_set_power_button_cb(power_monitor_power_button_cb_t power_button_cb, void * user_ctx);

#ifdef __cplusplus
}
#endif
