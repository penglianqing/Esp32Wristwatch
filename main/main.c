#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
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
    lv_obj_t * row_value_label;
    lv_obj_t * bars[HISTORY_COUNT];
    int32_t value;
    int32_t target;
    int32_t arc_value;
} metric_t;

static metric_t s_metrics[3];
static lv_obj_t * s_uptime_label;
static lv_obj_t * s_upload_label;
static lv_obj_t * s_download_label;
static lv_obj_t * s_bg_sweep[4];

static void set_metric_value(void * var, int32_t value)
{
    metric_t * metric = (metric_t *)var;
    metric->value = value;
    metric->target = value;

    /* 1 Hz text update: center big number and small CPU/MEM/GPU row number. */
    lv_label_set_text_fmt(metric->value_label, "%" LV_PRId32 "%s", value, metric->unit);
    if(metric->row_value_label != NULL) {
        lv_label_set_text_fmt(metric->row_value_label, "%" LV_PRId32 "%s", value, metric->unit);
    }
}

static void set_metric_arc_value(metric_t * metric, int32_t value)
{
    metric->arc_value = value;
    /* 30 Hz smooth ring update, independent from the text refresh rate. */
    lv_arc_set_value(metric->arc, value);
}

static int32_t next_value(int32_t current)
{
    int32_t drift = (int32_t)lv_rand(0, 38) - 19;
    int32_t target = current + drift;

    if(target < 8) target = 8 + (int32_t)lv_rand(0, 12);
    if(target > 94) target = 82 + (int32_t)lv_rand(0, 12);

    return target;
}

static int32_t next_speed(int32_t min, int32_t max)
{
    return min + (int32_t)lv_rand(0, (uint32_t)(max - min));
}

static void push_history(metric_t * metric, int32_t value)
{
    if(metric->bars[0] == NULL) {
        return;
    }

    /* Bottom history bars. Only CPU currently creates these bars. */
    for(int i = 0; i < HISTORY_COUNT - 1; i++) {
        int32_t old_value = lv_bar_get_value(metric->bars[i + 1]);
        lv_bar_set_value(metric->bars[i], old_value, LV_ANIM_OFF);
    }

    lv_bar_set_value(metric->bars[HISTORY_COUNT - 1], value, LV_ANIM_OFF);
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

            if(now_us - last_sample_us >= 1000000) {
                last_sample_us = now_us;
                for(int i = 0; i < 3; i++) {
                    set_metric_value(&s_metrics[i], next_value(s_metrics[i].value));
                }
                push_history(&s_metrics[0], s_metrics[0].target);

                int64_t sec = now_us / 1000000;
                lv_label_set_text_fmt(s_upload_label, "TX : %" LV_PRId32 "Mbps", next_speed(40, 180));
                lv_label_set_text_fmt(s_download_label, "RX : %" LV_PRId32 "Mbps", next_speed(120, 900));
                lv_label_set_text_fmt(s_uptime_label, "%02lld:%02lld", sec / 60, sec % 60);
            }

            for(int i = 0; i < 3; i++) {
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

            lv_refr_now(NULL);

            bsp_display_unlock();
        }
        else {
            ESP_LOGW(TAG, "LVGL lock timeout");
        }

        frame++;
        vTaskDelay(pdMS_TO_TICKS(33));
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
    /* Bottom sector mask: covers sweep arcs in the lower fan-shaped quiet zone. */
    lv_obj_t * mask = lv_arc_create(parent);
    lv_obj_set_size(mask, 430, 430);
    lv_obj_center(mask);
    lv_arc_set_bg_angles(mask, 90 - 25, 90 + 25);
    lv_arc_set_value(mask, 0);
    lv_obj_remove_style(mask, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mask, 440, 440); // 外半径 220
    lv_obj_set_style_arc_width(mask, 100, LV_PART_MAIN); // 内半径 120
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
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
    lv_obj_align(label, align, x, y);
    return label;
}

static void create_metric_ring(lv_obj_t * parent, metric_t * metric, const char * name,
                               const char * unit, lv_color_t color, int32_t size,
                               int32_t width, int32_t initial)
{
    metric->name = name;
    metric->unit = unit;
    metric->color = color;
    metric->value = initial;
    metric->target = initial;
    metric->arc_value = initial;

    /* Main circular CPU/MEM/GPU progress ring. */
    metric->arc = lv_arc_create(parent);
    lv_obj_set_size(metric->arc, size, size);
    lv_obj_center(metric->arc);
    lv_arc_set_range(metric->arc, 0, 100);
    lv_arc_set_rotation(metric->arc, 120);
    lv_arc_set_bg_angles(metric->arc, 0, 300);
    lv_arc_set_value(metric->arc, initial);
    lv_obj_remove_style(metric->arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(metric->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(metric->arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(metric->arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(metric->arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(metric->arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(metric->arc, lv_color_hex(0x1f2429), LV_PART_MAIN);
    lv_obj_set_style_arc_color(metric->arc, color, LV_PART_INDICATOR);
}

static lv_obj_t * create_sweep_arc(lv_obj_t * parent, int32_t size, int32_t width,
                                   lv_color_t color, int32_t angle)
{
    /* Decorative sweep/background arc, rotated every frame. */
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

static void create_metric_row(lv_obj_t * parent, metric_t * metric, lv_align_t align,
                              int32_t x, int32_t y)
{
    /* Small row: color dot + "CPU/MEM/GPU" + value like "61%". */
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, 152, 42);
    style_plain(row);
    lv_obj_align(row, align, x, y);

    lv_obj_t * dot = lv_obj_create(row);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, metric->color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, -7);

    create_label(row, metric->name, &lv_font_montserrat_16, lv_color_hex(0x9da5af),
                 LV_ALIGN_LEFT_MID, 18, -8);

    /* This label is the row value text, e.g. "61%". */
    metric->row_value_label = create_label(row, "", &lv_font_montserrat_22,
                                           lv_color_hex(0xf4f7fb), LV_ALIGN_LEFT_MID, 18, 12);
    lv_label_set_text_fmt(metric->row_value_label, "%" LV_PRId32 "%s", metric->value, metric->unit);
}

static void create_history(lv_obj_t * parent, metric_t * metric, int32_t y)
{
    /* Bottom mini bar history area. */
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

    /* Full 466 x 466 circular face for the round AMOLED. */
    lv_obj_t * face = lv_obj_create(screen);
    lv_obj_set_size(face, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(face);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(face, lv_color_hex(0x0c0f12), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(face, lv_color_hex(0x141018), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(face, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 0, LV_PART_MAIN);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);

    /* Background motion: outer sweep arcs plus one inner sweep arc. */
    s_bg_sweep[0] = create_sweep_arc(face, 450, 3, lv_color_hex(0x19d6b4), 0);
    s_bg_sweep[1] = create_sweep_arc(face, 365, 2, lv_color_hex(0xffc857), 120);
    s_bg_sweep[2] = create_sweep_arc(face, 286, 2, lv_color_hex(0xff4fa3), 240);
    s_bg_sweep[3] = create_sweep_arc(face, 212, 2, lv_color_hex(0xffffff), 180);
    lv_obj_set_style_arc_opa(s_bg_sweep[3], LV_OPA_30, LV_PART_MAIN);

    /* Main progress rings for CPU, MEM, GPU. */
    create_metric_ring(face, &s_metrics[0], "CPU", "%", lv_color_hex(0x19d6b4), 408, 16, 48);
    create_metric_ring(face, &s_metrics[1], "MEM", "%", lv_color_hex(0xffc857), 326, 14, 62);
    create_metric_ring(face, &s_metrics[2], "GPU", "%", lv_color_hex(0xff4fa3), 248, 12, 36);

    /* Center metric tags above the big values; the three groups are center aligned. */
    create_label(face, "CPU", &lv_font_montserrat_12, lv_color_hex(0x9da5af),
                 LV_ALIGN_CENTER, 0, -86);
    s_metrics[0].value_label = create_label(face, "", &lv_font_montserrat_26,
                                            s_metrics[0].color, LV_ALIGN_CENTER, 0, -62);

    create_label(face, "MEM", &lv_font_montserrat_12, lv_color_hex(0x9da5af),
                 LV_ALIGN_CENTER, 0, -18);
    s_metrics[1].value_label = create_label(face, "", &lv_font_montserrat_26,
                                            s_metrics[1].color, LV_ALIGN_CENTER, 0, 6);

    create_label(face, "GPU", &lv_font_montserrat_12, lv_color_hex(0x9da5af),
                 LV_ALIGN_CENTER, 0, 50);
    s_metrics[2].value_label = create_label(face, "", &lv_font_montserrat_24,
                                            s_metrics[2].color, LV_ALIGN_CENTER, 0, 74);

    lv_label_set_text_fmt(s_metrics[0].value_label, "%" LV_PRId32 "%s", s_metrics[0].value, s_metrics[0].unit);
    lv_label_set_text_fmt(s_metrics[1].value_label, "%" LV_PRId32 "%s", s_metrics[1].value, s_metrics[1].unit);
    lv_label_set_text_fmt(s_metrics[2].value_label, "%" LV_PRId32 "%s", s_metrics[2].value, s_metrics[2].unit);

    create_bottom_mask(face);

    /* Network throughput, centered above the CPU history bars. */
    s_upload_label = create_label(face, "TX : 100Mbps", &lv_font_montserrat_14,
                                  lv_color_hex(0x9da5af), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_TX_Y);
    s_download_label = create_label(face, "RX : 240Mbps", &lv_font_montserrat_14,
                                    lv_color_hex(0x9da5af), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_RX_Y);

    /* Bottom CPU-only history bars. */
    create_history(face, &s_metrics[0], BOTTOM_HISTORY_Y);

    /* Bottom uptime text and product name. */
    s_uptime_label = create_label(face, "00:00", &lv_font_montserrat_12,
                                  lv_color_hex(0xf5f7fb), LV_ALIGN_BOTTOM_MID, 0, BOTTOM_TIME_Y);
    create_label(face, "FnOS", &lv_font_montserrat_18, lv_color_hex(0xf5f7fb),
                 LV_ALIGN_BOTTOM_MID, 0, BOTTOM_BRAND_Y);

    for(int i = 0; i < 3; i++) {
        s_metrics[i].target = s_metrics[i].value;
        set_metric_value(&s_metrics[i], s_metrics[i].value);
        set_metric_arc_value(&s_metrics[i], s_metrics[i].value);
    }
}

void app_main(void)
{
    bsp_display_start();

    bsp_display_lock(-1);
    lv_rand_set_seed((uint32_t)esp_timer_get_time());
    create_dashboard();
    bsp_display_unlock();

    BaseType_t task_ok = xTaskCreate(dashboard_update_task, "dash_update", 8192, NULL, 8, NULL);
    if(task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create dashboard update task");
    }
}
