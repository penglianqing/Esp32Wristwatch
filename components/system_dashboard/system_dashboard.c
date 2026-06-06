#include "system_dashboard.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#define SCREEN_SIZE 466
#define HISTORY_COUNT 14
#define HISTORY_BAR_W ((HISTORY_COUNT) > 16 ? 8 : 7)
#define HISTORY_BAR_H ((HISTORY_COUNT) > 16 ? 24 : 22)
#define HISTORY_BAR_GAP ((HISTORY_COUNT) > 16 ? 4 : 3)
#define HISTORY_WRAP_W ((HISTORY_COUNT) * HISTORY_BAR_W + ((HISTORY_COUNT) - 1) * HISTORY_BAR_GAP)
#define BOTTOM_TX_Y (-95)
#define BOTTOM_RX_Y (-75)
#define BOTTOM_HISTORY_Y (-50)
#define BOTTOM_BRAND_Y (-22)
#define BOTTOM_TIME_Y (-2)

static const char *TAG = "sys_dash";

typedef struct {
    const char * name;
    const char * unit;
    lv_color_t color;
    lv_obj_t * arc;
    lv_obj_t * name_label;
    lv_obj_t * value_label;
    lv_obj_t * bars[HISTORY_COUNT];
    int32_t value;
    int32_t target;
    int32_t arc_value;
} metric_t;

static sys_dashboard_config_t s_config;
static metric_t s_metrics[SYS_DASHBOARD_METRIC_COUNT];
static lv_obj_t * s_uptime_label;
static lv_obj_t * s_upload_label;
static lv_obj_t * s_download_label;
static lv_obj_t * s_brand_label;
static lv_obj_t * s_bg_sweep[4];
static portMUX_TYPE s_value_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_metric_external_valid[SYS_DASHBOARD_PANEL_COUNT][SYS_DASHBOARD_METRIC_COUNT];
static int32_t s_metric_external_value[SYS_DASHBOARD_PANEL_COUNT][SYS_DASHBOARD_METRIC_COUNT];
static bool s_tx_external_valid[SYS_DASHBOARD_PANEL_COUNT];
static int32_t s_tx_external_value[SYS_DASHBOARD_PANEL_COUNT];
static bool s_rx_external_valid[SYS_DASHBOARD_PANEL_COUNT];
static int32_t s_rx_external_value[SYS_DASHBOARD_PANEL_COUNT];
static char s_weather_text[64];
static bool s_weather_temperature_valid;
static int32_t s_weather_temperature_c;
static int32_t s_clock_cpu_history_value;
static int32_t s_active_panel;

static int32_t clamp_value(int32_t value, int32_t min, int32_t max)
{
    if(value < min) return min;
    if(value > max) return max;
    return value;
}

static uint32_t frame_delay_ms(void)
{
    uint32_t hz = s_config.frame_refresh_hz ? s_config.frame_refresh_hz : 30;
    uint32_t delay = 1000 / hz;
    return delay ? delay : 1;
}

static uint32_t data_refresh_ms(void)
{
    return s_config.data_refresh_ms ? s_config.data_refresh_ms : 1000;
}

static const char * active_panel_name(void)
{
    const char * name = s_config.panel_names[s_active_panel];
    return name ? name : s_config.brand_name;
}

static bool clock_panel_active(void)
{
    return s_active_panel == 0;
}

static bool get_local_time(struct tm * timeinfo)
{
    time_t now = 0;
    time(&now);
    if(now < 1700000000) {
        int64_t sec = esp_timer_get_time() / 1000000;
        memset(timeinfo, 0, sizeof(*timeinfo));
        timeinfo->tm_hour = (int)((sec / 3600) % 24);
        timeinfo->tm_min = (int)((sec / 60) % 60);
        timeinfo->tm_sec = (int)(sec % 60);
        return false;
    }

    return localtime_r(&now, timeinfo) != NULL;
}

static int32_t clock_value(int32_t index)
{
    struct tm timeinfo = {0};
    get_local_time(&timeinfo);

    if(index == 0) return timeinfo.tm_hour;
    if(index == 1) return timeinfo.tm_min;
    return timeinfo.tm_sec;
}

static int32_t clock_arc_value(int32_t index, int32_t value)
{
    if(index == 0) return (value * 100) / 24;
    return (value * 100) / 60;
}

static bool parse_weather_temperature(const char * text, int32_t * out_celsius)
{
    if(text == NULL) {
        return false;
    }

    bool found = false;
    int32_t last_value = 0;
    const char * p = text;

    while(*p != '\0') {
        if((*p >= '0' && *p <= '9') ||
           ((*p == '-' || *p == '+') && p[1] >= '0' && p[1] <= '9')) {
            char * end = NULL;
            long value = strtol(p, &end, 10);
            if(end != p) {
                last_value = (int32_t)value;
                found = true;
                p = end;
                continue;
            }
        }
        p++;
    }

    if(found && out_celsius != NULL) {
        *out_celsius = last_value;
    }
    return found;
}

static const char * metric_title(int32_t index)
{
    static const char * clock_titles[SYS_DASHBOARD_METRIC_COUNT] = {"HOUR", "MIN", "SEC"};
    return clock_panel_active() ? clock_titles[index] : s_config.metrics[index].name;
}

static void set_metric_text(metric_t * metric, int32_t value)
{
    int32_t index = (int32_t)(metric - s_metrics);
    metric->value = value;
    if(clock_panel_active()) {
        metric->target = clock_arc_value(index, value);
        lv_label_set_text_fmt(metric->value_label, "%02" LV_PRId32, value);
    }
    else {
        metric->target = value;
        lv_label_set_text_fmt(metric->value_label, "%" LV_PRId32 "%s", value, metric->unit);
    }
}

static void set_metric_arc_value(metric_t * metric, int32_t value)
{
    metric->arc_value = value;
    lv_arc_set_value(metric->arc, value);
}

static int32_t next_value(const sys_dashboard_metric_config_t * cfg, int32_t current)
{
    int32_t drift = (int32_t)lv_rand(0, 38) - 19;
    int32_t target = current + drift;
    int32_t min = cfg->mock_min;
    int32_t max = cfg->mock_max;

    if(max <= min) {
        min = 0;
        max = 100;
    }

    if(target < min) target = min + (int32_t)lv_rand(0, 12);
    if(target > max) target = max - 12 + (int32_t)lv_rand(0, 12);
    if(target < min) target = min;
    if(target > max) target = max;

    return target;
}

static int32_t next_speed(const sys_dashboard_speed_config_t * speed)
{
    int32_t min = speed->mock_min;
    int32_t max = speed->mock_max;
    if(max <= min) {
        return speed->value;
    }

    return min + (int32_t)lv_rand(0, (uint32_t)(max - min));
}

static int32_t metric_sample(int32_t index)
{
    if(clock_panel_active()) {
        return clock_value(index);
    }

    int32_t value = 0;
    bool has_external = false;

    portENTER_CRITICAL(&s_value_lock);
    has_external = s_active_panel > 0 && s_metric_external_valid[s_active_panel][index];
    value = s_metric_external_value[s_active_panel][index];
    portEXIT_CRITICAL(&s_value_lock);

    if(has_external) {
        return clamp_value(value, 0, 100);
    }

    return next_value(&s_config.metrics[index], s_metrics[index].value);
}

static int32_t tx_sample(void)
{
    if(clock_panel_active()) {
        return 0;
    }

    int32_t value = 0;
    bool has_external = false;

    portENTER_CRITICAL(&s_value_lock);
    has_external = s_active_panel > 0 && s_tx_external_valid[s_active_panel];
    value = s_tx_external_value[s_active_panel];
    portEXIT_CRITICAL(&s_value_lock);

    return has_external ? value : next_speed(&s_config.tx);
}

static int32_t rx_sample(void)
{
    if(clock_panel_active()) {
        return 0;
    }

    int32_t value = 0;
    bool has_external = false;

    portENTER_CRITICAL(&s_value_lock);
    has_external = s_active_panel > 0 && s_rx_external_valid[s_active_panel];
    value = s_rx_external_value[s_active_panel];
    portEXIT_CRITICAL(&s_value_lock);

    return has_external ? value : next_speed(&s_config.rx);
}

static void push_history_bars(lv_obj_t ** bars, int32_t value)
{
    if(bars == NULL || bars[0] == NULL) {
        return;
    }

    for(int i = 0; i < HISTORY_COUNT - 1; i++) {
        int32_t old_value = lv_bar_get_value(bars[i + 1]);
        lv_bar_set_value(bars[i], old_value, LV_ANIM_OFF);
    }

    lv_bar_set_value(bars[HISTORY_COUNT - 1], value, LV_ANIM_OFF);
}

static void push_history(metric_t * metric, int32_t value)
{
    if(metric == NULL) {
        return;
    }

    push_history_bars(metric->bars, value);
}

static void set_history_color(metric_t * metric, lv_color_t color)
{
    if(metric == NULL || metric->bars[0] == NULL) {
        return;
    }

    for(int i = 0; i < HISTORY_COUNT; i++) {
        lv_obj_set_style_bg_color(metric->bars[i], color, LV_PART_INDICATOR);
    }
}

static void update_time_label(int64_t now_us)
{
    if(s_config.time_cb != NULL) {
        char time_buf[32];
        s_config.time_cb(time_buf, sizeof(time_buf), s_config.time_user_ctx);
        lv_label_set_text(s_uptime_label, time_buf);
    }
    else {
        int64_t sec = now_us / 1000000;
        lv_label_set_text_fmt(s_uptime_label, "%02lld:%02lld", sec / 60, sec % 60);
    }
}

static void update_metric_titles(void)
{
    for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
        if(s_metrics[i].name_label != NULL) {
            lv_label_set_text(s_metrics[i].name_label, metric_title(i));
        }
    }
}

static void update_bottom_labels(int64_t now_us)
{
    if(clock_panel_active()) {
        char weather_buf[64];
        struct tm timeinfo = {0};
        if(get_local_time(&timeinfo)) {
            char date_buf[32];
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %a", &timeinfo);
            lv_label_set_text(s_upload_label, date_buf);
        }
        else {
            int64_t sec = now_us / 1000000;
            lv_label_set_text_fmt(s_upload_label, "Uptime %02lld:%02lld:%02lld",
                                  sec / 3600, (sec / 60) % 60, sec % 60);
        }
        portENTER_CRITICAL(&s_value_lock);
        snprintf(weather_buf, sizeof(weather_buf), "%s", s_weather_text);
        portEXIT_CRITICAL(&s_value_lock);
        lv_label_set_text(s_download_label, weather_buf);
        return;
    }

    lv_label_set_text_fmt(s_upload_label, "%s : %" LV_PRId32 "%s",
                          s_config.tx.name, tx_sample(), s_config.tx.unit);
    lv_label_set_text_fmt(s_download_label, "%s : %" LV_PRId32 "%s",
                          s_config.rx.name, rx_sample(), s_config.rx.unit);
}

static void refresh_panel_label(void)
{
    if(s_brand_label != NULL) {
        lv_label_set_text(s_brand_label, active_panel_name());
    }
}

static void switch_panel(int32_t delta)
{
    int32_t next = (s_active_panel + delta) % SYS_DASHBOARD_PANEL_COUNT;
    if(next < 0) {
        next += SYS_DASHBOARD_PANEL_COUNT;
    }

    if(next == s_active_panel) {
        return;
    }

    s_active_panel = next;
    refresh_panel_label();
    update_metric_titles();
    for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
        set_metric_text(&s_metrics[i], metric_sample(i));
    }
    update_bottom_labels(esp_timer_get_time());
    ESP_LOGI(TAG, "panel switched to %s (%" LV_PRId32 ")",
             active_panel_name(), s_active_panel);
}

static void dashboard_gesture_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    lv_indev_t * indev = lv_indev_active();
    if(indev == NULL) {
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if(dir == LV_DIR_LEFT) {
        switch_panel(1);
    }
    else if(dir == LV_DIR_RIGHT) {
        switch_panel(-1);
    }
}

static void dashboard_update_task(void * arg)
{
    LV_UNUSED(arg);

    uint32_t frame = 0;
    int64_t last_sample_us = 0;
    ESP_LOGI(TAG, "dashboard update task started");

    while(1) {
        if(bsp_display_lock(500)) {
            int64_t now_us = esp_timer_get_time();

            if(now_us - last_sample_us >= (int64_t)data_refresh_ms() * 1000) {
                last_sample_us = now_us;
                for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
                    set_metric_text(&s_metrics[i], metric_sample(i));
                }

                if(s_config.history_metric_index >= 0 &&
                   s_config.history_metric_index < SYS_DASHBOARD_METRIC_COUNT) {
                    metric_t * history_metric = &s_metrics[s_config.history_metric_index];
                    if(clock_panel_active()) {
                        s_clock_cpu_history_value = next_value(&s_config.metrics[0], s_clock_cpu_history_value);
                        set_history_color(history_metric, s_config.metrics[0].color);
                        push_history(history_metric, s_clock_cpu_history_value);
                    }
                    else {
                        set_history_color(history_metric, history_metric->color);
                        push_history(history_metric, history_metric->target);
                    }
                }

                update_bottom_labels(now_us);
                update_time_label(now_us);
            }

            for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
                int32_t delta = s_metrics[i].target - s_metrics[i].arc_value;
                int32_t step = delta / 8;

                if(step == 0 && delta != 0) {
                    step = delta > 0 ? 1 : -1;
                }

                set_metric_arc_value(&s_metrics[i], s_metrics[i].arc_value + step);
            }

            for(int i = 0; i < 3; i++) {
                lv_arc_set_rotation(s_bg_sweep[i], (int32_t)((frame * (2 + i)) + (i * 120)) % 360);
            }
            lv_arc_set_rotation(s_bg_sweep[3], (int32_t)((frame * 5) + 180) % 360);

            bsp_display_unlock();
        }
        else {
            ESP_LOGW(TAG, "LVGL lock timeout");
        }

        frame++;
        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms()));
    }
}

static void style_plain(lv_obj_t * obj)
{
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
}

static lv_obj_t * create_bottom_mask(lv_obj_t * parent)
{
    lv_obj_t * mask = lv_arc_create(parent);
    lv_obj_set_size(mask, 440, 440);
    lv_obj_center(mask);
    lv_arc_set_bg_angles(mask, 65, 115);
    lv_arc_set_value(mask, 0);
    lv_obj_remove_style(mask, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(mask, 100, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mask, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(mask, false, LV_PART_MAIN);
    lv_obj_set_style_arc_color(mask, lv_color_hex(0x0c0f12), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(mask, LV_OPA_COVER, LV_PART_MAIN);
    return mask;
}

static lv_obj_t * create_label(lv_obj_t * parent, const char * text, const lv_font_t * font,
                               lv_color_t color, lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font ? font : LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
    lv_obj_align(label, align, x, y);
    return label;
}

static void create_metric_ring(lv_obj_t * parent, metric_t * metric,
                               const sys_dashboard_metric_config_t * cfg)
{
    metric->name = cfg->name;
    metric->unit = cfg->unit;
    metric->color = cfg->color;
    metric->value = cfg->value;
    metric->target = cfg->value;
    metric->arc_value = cfg->value;

    metric->arc = lv_arc_create(parent);
    lv_obj_set_size(metric->arc, cfg->ring_size, cfg->ring_size);
    lv_obj_center(metric->arc);
    lv_arc_set_range(metric->arc, 0, 100);
    lv_arc_set_rotation(metric->arc, 120);
    lv_arc_set_bg_angles(metric->arc, 0, 300);
    lv_arc_set_value(metric->arc, cfg->value);
    lv_obj_remove_style(metric->arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(metric->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(metric->arc, cfg->ring_width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(metric->arc, cfg->ring_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(metric->arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(metric->arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(metric->arc, lv_color_hex(0x1f2429), LV_PART_MAIN);
    lv_obj_set_style_arc_color(metric->arc, cfg->color, LV_PART_INDICATOR);
}

static lv_obj_t * create_sweep_arc(lv_obj_t * parent, int32_t size, int32_t width,
                                   lv_color_t color, int32_t angle)
{
    lv_obj_t * arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_rotation(arc, angle);
    lv_arc_set_bg_angles(arc, 0, 64);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_50, LV_PART_MAIN);
    return arc;
}

static void create_history(lv_obj_t * parent, metric_t * metric, int32_t y)
{
    lv_obj_t * wrap = lv_obj_create(parent);
    lv_obj_set_size(wrap, HISTORY_WRAP_W, HISTORY_BAR_H);
    style_plain(wrap);
    lv_obj_align(wrap, LV_ALIGN_BOTTOM_MID, 0, y);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wrap, HISTORY_BAR_GAP, LV_PART_MAIN);

    for(int i = 0; i < HISTORY_COUNT; i++) {
        metric->bars[i] = lv_bar_create(wrap);
        lv_obj_set_size(metric->bars[i], HISTORY_BAR_W, HISTORY_BAR_H);
        lv_bar_set_range(metric->bars[i], 0, 100);
        lv_bar_set_value(metric->bars[i], metric->value, LV_ANIM_OFF);
        lv_obj_set_style_radius(metric->bars[i], 4, LV_PART_MAIN);
        lv_obj_set_style_radius(metric->bars[i], 4, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(metric->bars[i], lv_color_hex(0x171b1f), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(metric->bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(metric->bars[i], metric->color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(metric->bars[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(metric->bars[i], 0, LV_PART_MAIN);
    }
}

static void create_dashboard(void)
{
    lv_obj_t * screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080a0c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, dashboard_gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t * face = lv_obj_create(screen);
    lv_obj_set_size(face, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(face);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(face, lv_color_hex(0x0c0f12), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(face, lv_color_hex(0x0c0f12), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(face, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 0, LV_PART_MAIN);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(face, dashboard_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_bg_sweep[0] = create_sweep_arc(face, 450, 3, lv_color_hex(0x19d6b4), 0);
    s_bg_sweep[1] = create_sweep_arc(face, 365, 2, lv_color_hex(0xffc857), 120);
    s_bg_sweep[2] = create_sweep_arc(face, 286, 2, lv_color_hex(0xff4fa3), 240);
    s_bg_sweep[3] = create_sweep_arc(face, 212, 2, lv_color_hex(0xffffff), 180);
    lv_obj_set_style_arc_opa(s_bg_sweep[3], LV_OPA_30, LV_PART_MAIN);

    for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
        create_metric_ring(face, &s_metrics[i], &s_config.metrics[i]);
    }

    const int32_t tag_y[SYS_DASHBOARD_METRIC_COUNT] = {-86, -18, 50};
    const int32_t value_y[SYS_DASHBOARD_METRIC_COUNT] = {-62, 6, 74};
    for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
        s_metrics[i].name_label = create_label(face, metric_title(i), &lv_font_montserrat_12,
                                               lv_color_hex(0x9da5af), LV_ALIGN_CENTER, 0, tag_y[i]);
        s_metrics[i].value_label = create_label(face, "", s_config.metrics[i].value_font,
                                                s_metrics[i].color, LV_ALIGN_CENTER, 0, value_y[i]);
        set_metric_text(&s_metrics[i], metric_sample(i));
        set_metric_arc_value(&s_metrics[i], s_metrics[i].target);
    }

    create_bottom_mask(face);

    s_upload_label = create_label(face, "", &lv_font_montserrat_14,
                                  lv_color_hex(0x9da5af), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_TX_Y);
    s_download_label = create_label(face, "", &lv_font_montserrat_14,
                                    lv_color_hex(0x9da5af), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_RX_Y);
    update_bottom_labels(esp_timer_get_time());

    if(s_config.history_metric_index >= 0 &&
       s_config.history_metric_index < SYS_DASHBOARD_METRIC_COUNT) {
        create_history(face, &s_metrics[s_config.history_metric_index], BOTTOM_HISTORY_Y);
    }

    s_uptime_label = create_label(face, s_config.time_text, &lv_font_montserrat_12,
                                  lv_color_hex(0xf5f7fb), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_TIME_Y);
    update_time_label(esp_timer_get_time());
    s_brand_label = create_label(face, active_panel_name(), &lv_font_montserrat_18, lv_color_hex(0xf5f7fb),
                                 LV_ALIGN_BOTTOM_MID, 0, BOTTOM_BRAND_Y);
}

void sys_dashboard_start(const sys_dashboard_config_t * config)
{
    if(config == NULL) {
        ESP_LOGE(TAG, "missing dashboard config");
        return;
    }

    memcpy(&s_config, config, sizeof(s_config));
    if(s_config.brand_name == NULL) s_config.brand_name = "";
    if(s_config.weather_text == NULL) s_config.weather_text = "Beijing --";
    snprintf(s_weather_text, sizeof(s_weather_text), "%s", s_config.weather_text);
    int32_t initial_temp_c = 0;
    if(parse_weather_temperature(s_config.weather_text, &initial_temp_c)) {
        s_weather_temperature_c = initial_temp_c;
        s_weather_temperature_valid = true;
    }
    if(s_config.default_panel_index < 0 || s_config.default_panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
        s_config.default_panel_index = 0;
    }
    s_clock_cpu_history_value = s_config.metrics[0].value;
    s_active_panel = s_config.default_panel_index;
    if(s_config.panel_names[0] == NULL) s_config.panel_names[0] = s_config.brand_name;
    if(s_config.panel_names[1] == NULL) s_config.panel_names[1] = "FnOS";
    if(s_config.panel_names[2] == NULL) s_config.panel_names[2] = "Windows11";
    if(s_config.time_text == NULL) s_config.time_text = "00:00";
    if(s_config.tx.name == NULL) s_config.tx.name = "TX";
    if(s_config.tx.unit == NULL) s_config.tx.unit = "Mbps";
    if(s_config.rx.name == NULL) s_config.rx.name = "RX";
    if(s_config.rx.unit == NULL) s_config.rx.unit = "Mbps";

    for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
        if(s_config.metrics[i].name == NULL) s_config.metrics[i].name = "";
        if(s_config.metrics[i].unit == NULL) s_config.metrics[i].unit = "%";
        if(s_config.metrics[i].ring_size <= 0) s_config.metrics[i].ring_size = 248;
        if(s_config.metrics[i].ring_width <= 0) s_config.metrics[i].ring_width = 12;
        if(s_config.metrics[i].mock_max <= s_config.metrics[i].mock_min) {
            s_config.metrics[i].mock_min = 0;
            s_config.metrics[i].mock_max = 100;
        }
        if(s_config.metrics[i].value_font == NULL) s_config.metrics[i].value_font = &lv_font_montserrat_24;
    }

    if(bsp_display_lock(1000)) {
        create_dashboard();
        bsp_display_unlock();
    }
    else {
        ESP_LOGE(TAG, "failed to lock display while creating dashboard");
        return;
    }

    BaseType_t task_ok = xTaskCreate(dashboard_update_task, "dash_update", 8192, NULL, 8, NULL);
    if(task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create dashboard update task");
    }
}

void sys_dashboard_set_panel_metric_value(int32_t panel_index, int32_t metric_index, int32_t value)
{
    if(panel_index <= 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT ||
       metric_index < 0 || metric_index >= SYS_DASHBOARD_METRIC_COUNT) {
        return;
    }

    portENTER_CRITICAL(&s_value_lock);
    s_metric_external_value[panel_index][metric_index] = value;
    s_metric_external_valid[panel_index][metric_index] = true;
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_set_panel_tx_value(int32_t panel_index, int32_t value)
{
    if(panel_index <= 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
        return;
    }

    portENTER_CRITICAL(&s_value_lock);
    s_tx_external_value[panel_index] = value;
    s_tx_external_valid[panel_index] = true;
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_set_panel_rx_value(int32_t panel_index, int32_t value)
{
    if(panel_index <= 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
        return;
    }

    portENTER_CRITICAL(&s_value_lock);
    s_rx_external_value[panel_index] = value;
    s_rx_external_valid[panel_index] = true;
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_set_weather_text(const char * text)
{
    if(text == NULL || text[0] == '\0') {
        return;
    }

    int32_t temp_c = 0;
    bool has_temp = parse_weather_temperature(text, &temp_c);

    portENTER_CRITICAL(&s_value_lock);
    snprintf(s_weather_text, sizeof(s_weather_text), "%s", text);
    if(has_temp) {
        s_weather_temperature_c = temp_c;
        s_weather_temperature_valid = true;
    }
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_set_weather_temperature(int32_t celsius)
{
    portENTER_CRITICAL(&s_value_lock);
    s_weather_temperature_c = celsius;
    s_weather_temperature_valid = true;
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_set_metric_value(int32_t index, int32_t value)
{
    sys_dashboard_set_panel_metric_value(1, index, value);
}

void sys_dashboard_set_tx_value(int32_t value)
{
    sys_dashboard_set_panel_tx_value(1, value);
}

void sys_dashboard_set_rx_value(int32_t value)
{
    sys_dashboard_set_panel_rx_value(1, value);
}
