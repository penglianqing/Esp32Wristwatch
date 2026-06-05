#pragma once

#include <stdint.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_DASHBOARD_METRIC_COUNT 3

typedef void (*sys_dashboard_time_cb_t)(char * buf, size_t buf_size, void * user_ctx);

typedef struct {
    const char * name;
    const char * unit;
    int32_t value;
    lv_color_t color;
    int32_t ring_size;
    int32_t ring_width;
    int32_t mock_min;
    int32_t mock_max;
    const lv_font_t * value_font;
} sys_dashboard_metric_config_t;

typedef struct {
    const char * name;
    const char * unit;
    int32_t value;
    int32_t mock_min;
    int32_t mock_max;
} sys_dashboard_speed_config_t;

typedef struct {
    uint32_t data_refresh_ms;
    uint32_t frame_refresh_hz;
    const char * brand_name;
    const char * time_text;
    sys_dashboard_time_cb_t time_cb;
    void * time_user_ctx;
    int32_t history_metric_index;
    sys_dashboard_metric_config_t metrics[SYS_DASHBOARD_METRIC_COUNT];
    sys_dashboard_speed_config_t tx;
    sys_dashboard_speed_config_t rx;
} sys_dashboard_config_t;

void sys_dashboard_start(const sys_dashboard_config_t * config);

#ifdef __cplusplus
}
#endif
