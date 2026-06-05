#include "system_dashboard.h"

#include <string.h>

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
static lv_obj_t * s_bg_sweep[4];

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

static void set_metric_text(metric_t * metric, int32_t value)
{
    metric->value = value;
    metric->target = value;
    lv_label_set_text_fmt(metric->value_label, "%" LV_PRId32 "%s", value, metric->unit);
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

static void push_history(metric_t * metric, int32_t value)
{
    if(metric == NULL || metric->bars[0] == NULL) {
        return;
    }

    for(int i = 0; i < HISTORY_COUNT - 1; i++) {
        int32_t old_value = lv_bar_get_value(metric->bars[i + 1]);
        lv_bar_set_value(metric->bars[i], old_value, LV_ANIM_OFF);
    }

    lv_bar_set_value(metric->bars[HISTORY_COUNT - 1], value, LV_ANIM_OFF);
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
                    set_metric_text(&s_metrics[i], next_value(&s_config.metrics[i], s_metrics[i].value));
                }

                if(s_config.history_metric_index >= 0 &&
                   s_config.history_metric_index < SYS_DASHBOARD_METRIC_COUNT) {
                    metric_t * history_metric = &s_metrics[s_config.history_metric_index];
                    push_history(history_metric, history_metric->target);
                }

                lv_label_set_text_fmt(s_upload_label, "%s : %" LV_PRId32 "%s",
                                      s_config.tx.name, next_speed(&s_config.tx), s_config.tx.unit);
                lv_label_set_text_fmt(s_download_label, "%s : %" LV_PRId32 "%s",
                                      s_config.rx.name, next_speed(&s_config.rx), s_config.rx.unit);
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
        create_label(face, s_metrics[i].name, &lv_font_montserrat_12, lv_color_hex(0x9da5af),
                     LV_ALIGN_CENTER, 0, tag_y[i]);
        s_metrics[i].value_label = create_label(face, "", s_config.metrics[i].value_font,
                                                s_metrics[i].color, LV_ALIGN_CENTER, 0, value_y[i]);
        set_metric_text(&s_metrics[i], s_metrics[i].value);
        set_metric_arc_value(&s_metrics[i], s_metrics[i].value);
    }

    create_bottom_mask(face);

    s_upload_label = create_label(face, "", &lv_font_montserrat_14,
                                  lv_color_hex(0x9da5af), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_TX_Y);
    s_download_label = create_label(face, "", &lv_font_montserrat_14,
                                    lv_color_hex(0x9da5af), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_RX_Y);
    lv_label_set_text_fmt(s_upload_label, "%s : %" LV_PRId32 "%s",
                          s_config.tx.name, s_config.tx.value, s_config.tx.unit);
    lv_label_set_text_fmt(s_download_label, "%s : %" LV_PRId32 "%s",
                          s_config.rx.name, s_config.rx.value, s_config.rx.unit);

    if(s_config.history_metric_index >= 0 &&
       s_config.history_metric_index < SYS_DASHBOARD_METRIC_COUNT) {
        create_history(face, &s_metrics[s_config.history_metric_index], BOTTOM_HISTORY_Y);
    }

    s_uptime_label = create_label(face, s_config.time_text, &lv_font_montserrat_12,
                                  lv_color_hex(0xf5f7fb), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_TIME_Y);
    update_time_label(esp_timer_get_time());
    create_label(face, s_config.brand_name, &lv_font_montserrat_18, lv_color_hex(0xf5f7fb),
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
