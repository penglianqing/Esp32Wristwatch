#include "system_dashboard.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#define SCREEN_SIZE 466

// HA Dashboard
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
#define TOP_BATTERY_Y (-2)

// Photo Dashboard
#define PHOTO_IMAGE_BYTES ((SCREEN_SIZE) * (SCREEN_SIZE) * 2)
#define PHOTO_STRIPE_HEIGHT 8
#define PHOTO_STRIPE_COUNT (((SCREEN_SIZE) + (PHOTO_STRIPE_HEIGHT) - 1) / (PHOTO_STRIPE_HEIGHT))
#define PHOTO_STRIPES_PER_BATCH 2
#define PHOTO_FRAME_DELAY_MS 5
#define DASHBOARD_FRAME_DELAY_MS 33
#define DASHBOARD_SWEEP_FRAME_DIVIDER 1
#define PANEL_TRANSITION_HOLD_FRAMES 2
#define PANEL_TRANSITION_BLACK_DELAY_MS 20
#define PHOTO_BATTERY_Y (-210)
#define PHOTO_TIME_Y (120)
#define PHOTO_DATE_Y (160)
#define PHOTO_WEATHER_Y (200)

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
static lv_obj_t * s_battery_label;
static lv_obj_t * s_face;
static lv_obj_t * s_photo_page;
static lv_obj_t * s_photo_stripes[PHOTO_STRIPE_COUNT];
static lv_obj_t * s_photo_placeholder_label;
static lv_obj_t * s_photo_time_label;
static lv_obj_t * s_photo_date_label;
static lv_obj_t * s_photo_battery_label;
static lv_obj_t * s_photo_weather_label;
static lv_obj_t * s_bg_sweep[4];
static lv_image_dsc_t s_photo_dsc[PHOTO_STRIPE_COUNT];
static uint8_t * s_photo_buf;
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
static int32_t s_battery_percent = -1;
static int32_t s_pending_panel_delta;
static int32_t s_pending_panel_index = -1;
static bool s_photo_memory_pending;
static bool s_photo_apply_pending;
static int32_t s_photo_apply_index;
static bool s_panel_build_pending;
static bool s_panel_black_frame_pending;
static lv_obj_t * s_panel_staging_screen;
static uint8_t s_panel_transition_hold_frames;
static int64_t s_last_lvgl_lock_timeout_log_us;
static const uint8_t * s_photo_fallback_data;
static size_t s_photo_fallback_len;
static char s_last_photo_time_text[16];
static char s_last_photo_date_text[32];
static char s_last_photo_battery_text[16];
static char s_last_photo_weather_text[64];

static void reset_dashboard_face_refs(void)
{
    s_face = NULL;
    s_uptime_label = NULL;
    s_upload_label = NULL;
    s_download_label = NULL;
    s_brand_label = NULL;
    s_battery_label = NULL;
    for(int i = 0; i < 4; i++) {
        s_bg_sweep[i] = NULL;
    }
    for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
        s_metrics[i].arc = NULL;
        s_metrics[i].name_label = NULL;
        s_metrics[i].value_label = NULL;
        for(int j = 0; j < HISTORY_COUNT; j++) {
            s_metrics[i].bars[j] = NULL;
        }
    }
}

static void reset_photo_page_refs(void)
{
    s_photo_page = NULL;
    s_photo_placeholder_label = NULL;
    s_photo_time_label = NULL;
    s_photo_date_label = NULL;
    s_photo_battery_label = NULL;
    s_photo_weather_label = NULL;
    for(int i = 0; i < PHOTO_STRIPE_COUNT; i++) {
        s_photo_stripes[i] = NULL;
        memset(&s_photo_dsc[i], 0, sizeof(s_photo_dsc[i]));
    }
}

static int32_t clamp_value(int32_t value, int32_t min, int32_t max)
{
    if(value < min) return min;
    if(value > max) return max;
    return value;
}

static uint32_t frame_delay_ms(void)
{
    if(s_active_panel == SYS_DASHBOARD_PANEL_COUNT - 1) {
        return PHOTO_FRAME_DELAY_MS;
    }
    uint32_t hz = s_config.frame_refresh_hz ? s_config.frame_refresh_hz : 30;
    uint32_t delay = 1000 / hz;
    if(delay < DASHBOARD_FRAME_DELAY_MS) {
        delay = DASHBOARD_FRAME_DELAY_MS;
    }
    return delay ? delay : DASHBOARD_FRAME_DELAY_MS;
}

static uint32_t data_refresh_ms(void)
{
    return s_config.data_refresh_ms ? s_config.data_refresh_ms : 1000;
}

static int32_t active_panel_index(void)
{
#if SYS_DASHBOARD_PANEL_COUNT == 1
    return 0;
#else
    return clamp_value(s_active_panel, 0, SYS_DASHBOARD_PANEL_COUNT - 1);
#endif
}

static const char * active_panel_name(void)
{
    const char * name = s_config.panel_names[active_panel_index()];
    return name ? name : s_config.brand_name;
}

static bool clock_panel_active(void)
{
    return false;
}

static bool photo_panel_active(void)
{
    return s_active_panel == SYS_DASHBOARD_PANEL_COUNT - 1;
}

static void detach_photo_stripes(void)
{
    s_photo_apply_pending = false;
    s_photo_apply_index = 0;

    for(int i = 0; i < PHOTO_STRIPE_COUNT; i++) {
        if(s_photo_stripes[i] != NULL) {
            lv_image_set_src(s_photo_stripes[i], NULL);
            lv_obj_add_flag(s_photo_stripes[i], LV_OBJ_FLAG_HIDDEN);
        }
        memset(&s_photo_dsc[i], 0, sizeof(s_photo_dsc[i]));
    }
}

static void release_photo_buffer(void)
{
    detach_photo_stripes();

    if(s_photo_buf != NULL) {
        heap_caps_free(s_photo_buf);
        s_photo_buf = NULL;
    }

    s_last_photo_time_text[0] = '\0';
    s_last_photo_date_text[0] = '\0';
    s_last_photo_battery_text[0] = '\0';

    portENTER_CRITICAL(&s_value_lock);
    s_photo_memory_pending = false;
    portEXIT_CRITICAL(&s_value_lock);
}

static void create_photo_page(lv_obj_t * screen);
static void create_dashboard_face(lv_obj_t * screen);
static void dashboard_gesture_cb(lv_event_t * event);
static lv_obj_t * create_screen_background(lv_obj_t * parent);
static lv_obj_t * create_panel_screen(void);
static void populate_panel_screen(lv_obj_t * screen);
static void enable_gesture_bubble(lv_obj_t * obj);
static void update_metric_titles(void);
static void refresh_panel_label(void);
static int32_t metric_sample(int32_t index);
static void update_bottom_labels(int64_t now_us);
static void set_metric_text(metric_t * metric, int32_t value);
static bool set_label_text_if_changed(lv_obj_t * label, char * cache, size_t cache_size, const char * text);
static bool metric_has_external_data(int32_t panel_index, int32_t metric_index, int32_t * out_value);
static bool tx_has_external_data(int32_t panel_index, int32_t * out_value);
static bool rx_has_external_data(int32_t panel_index, int32_t * out_value);
static const uint8_t * current_photo_pixels(void);

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

static void format_weather_display_text(const char * raw_text, char * out_buf, size_t out_buf_size)
{
    if(out_buf == NULL || out_buf_size == 0) {
        return;
    }

    out_buf[0] = '\0';
    if(raw_text == NULL || raw_text[0] == '\0') {
        snprintf(out_buf, out_buf_size, "%s", "--");
        return;
    }

    const char * text = raw_text;
    if(strncmp(text, "Beijing", 7) == 0) {
        text += 7;
        while(*text == ' ' || *text == ',' || *text == ':' || *text == '-' || *text == '/') {
            text++;
        }
    }

    if(*text == '\0') {
        snprintf(out_buf, out_buf_size, "%s", "--");
        return;
    }

    snprintf(out_buf, out_buf_size, "%s", text);
}

static const char * metric_title(int32_t index)
{
    static const char * clock_titles[SYS_DASHBOARD_METRIC_COUNT] = {"HOUR", "MIN", "SEC"};
    return clock_panel_active() ? clock_titles[index] : s_config.metrics[index].name;
}

static lv_obj_t * create_panel_screen(void)
{
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080a0c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, dashboard_gesture_cb, LV_EVENT_GESTURE, NULL);
    create_screen_background(screen);

    return screen;
}

static void populate_panel_screen(lv_obj_t * screen)
{
    if(screen == NULL) {
        return;
    }

    if(photo_panel_active()) {
        create_photo_page(screen);
    }
    else {
        create_dashboard_face(screen);
    }
}

static void begin_panel_transition(void)
{
    /* Screens are rebuilt on dedicated LVGL screens; transition work is handled
     * by the staged blank-screen/load path in switch_panel()/show_panel(). */
}

static void finish_pending_panel_build(void)
{
    if(!s_panel_build_pending) {
        return;
    }

    if(s_panel_transition_hold_frames > 0) {
        s_panel_transition_hold_frames--;
        return;
    }

    if(s_panel_black_frame_pending) {
        s_panel_black_frame_pending = false;
        s_panel_transition_hold_frames = PANEL_TRANSITION_HOLD_FRAMES;
        return;
    }

    lv_obj_t * active_screen = lv_screen_active();
    populate_panel_screen(active_screen);
    s_panel_staging_screen = active_screen;
    s_panel_build_pending = false;
    s_panel_transition_hold_frames = PANEL_TRANSITION_HOLD_FRAMES;
}

static bool metric_has_external_data(int32_t panel_index, int32_t metric_index, int32_t * out_value)
{
    if(panel_index < 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT ||
       metric_index < 0 || metric_index >= SYS_DASHBOARD_METRIC_COUNT) {
        return false;
    }

    bool has_external = false;
    int32_t value = 0;
    portENTER_CRITICAL(&s_value_lock);
    has_external = s_metric_external_valid[panel_index][metric_index];
    value = s_metric_external_value[panel_index][metric_index];
    portEXIT_CRITICAL(&s_value_lock);

    if(has_external && out_value != NULL) {
        *out_value = value;
    }
    return has_external;
}

static bool tx_has_external_data(int32_t panel_index, int32_t * out_value)
{
    if(panel_index < 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
        return false;
    }

    bool has_external = false;
    int32_t value = 0;
    portENTER_CRITICAL(&s_value_lock);
    has_external = s_tx_external_valid[panel_index];
    value = s_tx_external_value[panel_index];
    portEXIT_CRITICAL(&s_value_lock);

    if(has_external && out_value != NULL) {
        *out_value = value;
    }
    return has_external;
}

static bool rx_has_external_data(int32_t panel_index, int32_t * out_value)
{
    if(panel_index < 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
        return false;
    }

    bool has_external = false;
    int32_t value = 0;
    portENTER_CRITICAL(&s_value_lock);
    has_external = s_rx_external_valid[panel_index];
    value = s_rx_external_value[panel_index];
    portEXIT_CRITICAL(&s_value_lock);

    if(has_external && out_value != NULL) {
        *out_value = value;
    }
    return has_external;
}

static const uint8_t * current_photo_pixels(void)
{
    if(s_photo_buf != NULL) {
        return s_photo_buf;
    }

    if(s_photo_fallback_data != NULL && s_photo_fallback_len == PHOTO_IMAGE_BYTES) {
        return s_photo_fallback_data;
    }

    return NULL;
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
        if(value < 0) {
            metric->target = 0;
            lv_label_set_text(metric->value_label, "-");
        }
        else {
            metric->target = value;
            lv_label_set_text_fmt(metric->value_label, "%" LV_PRId32 "%s", value, metric->unit);
        }
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

    int32_t panel_index = active_panel_index();
    int32_t value = 0;

    if(metric_has_external_data(panel_index, index, &value)) {
        return clamp_value(value, 0, 100);
    }

    return -1;
}

static int32_t tx_sample(void)
{
    if(clock_panel_active()) {
        return 0;
    }

    int32_t panel_index = active_panel_index();
    int32_t value = 0;
    return tx_has_external_data(panel_index, &value) ? value : -1;
}

static int32_t rx_sample(void)
{
    if(clock_panel_active()) {
        return 0;
    }

    int32_t panel_index = active_panel_index();
    int32_t value = 0;
    return rx_has_external_data(panel_index, &value) ? value : -1;
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
        format_weather_display_text(s_weather_text, weather_buf, sizeof(weather_buf));
        portEXIT_CRITICAL(&s_value_lock);
        lv_label_set_text(s_download_label, weather_buf);
        return;
    }

    int32_t tx_value = tx_sample();
    int32_t rx_value = rx_sample();
    if(tx_value < 0) {
        lv_label_set_text_fmt(s_upload_label, "%s : -", s_config.tx.name);
    }
    else {
        lv_label_set_text_fmt(s_upload_label, "%s : %" LV_PRId32 "%s",
                              s_config.tx.name, tx_value, s_config.tx.unit);
    }
    if(rx_value < 0) {
        lv_label_set_text_fmt(s_download_label, "%s : -", s_config.rx.name);
    }
    else {
        lv_label_set_text_fmt(s_download_label, "%s : %" LV_PRId32 "%s",
                              s_config.rx.name, rx_value, s_config.rx.unit);
    }
}

static void refresh_panel_label(void)
{
    if(s_brand_label != NULL) {
        lv_label_set_text(s_brand_label, active_panel_name());
    }
}

static lv_obj_t * create_screen_background(lv_obj_t * parent)
{
    lv_obj_t * bg = lv_obj_create(parent);
    enable_gesture_bubble(bg);
    lv_obj_set_size(bg, lv_display_get_horizontal_resolution(NULL),
                    lv_display_get_vertical_resolution(NULL));
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(bg, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x080a0c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bg, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bg, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    return bg;
}

static bool set_label_text_if_changed(lv_obj_t * label, char * cache, size_t cache_size, const char * text)
{
    if(label == NULL || cache == NULL || cache_size == 0 || text == NULL) {
        return false;
    }

    if(strncmp(cache, text, cache_size) == 0) {
        return false;
    }

    snprintf(cache, cache_size, "%s", text);
    lv_label_set_text(label, cache);
    return true;
}

static void update_battery_label(void)
{
    if(s_battery_label == NULL) {
        return;
    }

    if(s_battery_percent < 0) {
        lv_label_set_text(s_battery_label, "--%");
        return;
    }

    lv_label_set_text_fmt(s_battery_label, " %" LV_PRId32 "%%", s_battery_percent);
}

static void update_photo_labels(void)
{
    if(s_photo_time_label == NULL || s_photo_date_label == NULL ||
       s_photo_battery_label == NULL || s_photo_weather_label == NULL) {
        return;
    }

    char time_buf[16];
    char date_buf[32];
    char battery_buf[16];
    char weather_buf[64];

    struct tm timeinfo = {0};
    if(get_local_time(&timeinfo)) {
        strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %a", &timeinfo);
    }
    else {
        snprintf(time_buf, sizeof(time_buf), "%s", "--:--");
        snprintf(date_buf, sizeof(date_buf), "%s", "---- -- --");
    }

    if(s_battery_percent < 0) {
        snprintf(battery_buf, sizeof(battery_buf), "%s", "-- %");
    }
    else {
        snprintf(battery_buf, sizeof(battery_buf), " %" LV_PRId32 "%%", s_battery_percent);
    }

    portENTER_CRITICAL(&s_value_lock);
    format_weather_display_text(s_weather_text, weather_buf, sizeof(weather_buf));
    portEXIT_CRITICAL(&s_value_lock);

    set_label_text_if_changed(s_photo_time_label, s_last_photo_time_text, sizeof(s_last_photo_time_text), time_buf);
    set_label_text_if_changed(s_photo_date_label, s_last_photo_date_text, sizeof(s_last_photo_date_text), date_buf);
    set_label_text_if_changed(s_photo_battery_label, s_last_photo_battery_text, sizeof(s_last_photo_battery_text), battery_buf);
    set_label_text_if_changed(s_photo_weather_label, s_last_photo_weather_text, sizeof(s_last_photo_weather_text), weather_buf);
}

static bool apply_staged_photo(void)
{
    const uint8_t * photo_pixels = current_photo_pixels();
    if(s_photo_stripes[0] == NULL || photo_pixels == NULL) {
        if(s_photo_placeholder_label != NULL) {
            lv_obj_remove_flag(s_photo_placeholder_label, LV_OBJ_FLAG_HIDDEN);
        }
        return false;
    }

    for(int i = 0; i < PHOTO_STRIPE_COUNT; i++) {
        int32_t stripe_y = i * PHOTO_STRIPE_HEIGHT;
        int32_t stripe_h = PHOTO_STRIPE_HEIGHT;
        if((stripe_y + stripe_h) > SCREEN_SIZE) {
            stripe_h = SCREEN_SIZE - stripe_y;
        }

        memset(&s_photo_dsc[i], 0, sizeof(s_photo_dsc[i]));
        s_photo_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_photo_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_photo_dsc[i].header.w = SCREEN_SIZE;
        s_photo_dsc[i].header.h = stripe_h;
        s_photo_dsc[i].header.stride = SCREEN_SIZE * 2;
        s_photo_dsc[i].data_size = SCREEN_SIZE * stripe_h * 2;
        s_photo_dsc[i].data = photo_pixels + (stripe_y * SCREEN_SIZE * 2);
    }

    s_photo_apply_index = 0;
    s_photo_apply_pending = true;
    return true;
}

static void apply_photo_stripe_batch(void)
{
    if(!s_photo_apply_pending || s_photo_stripes[0] == NULL) {
        return;
    }

    int32_t end = s_photo_apply_index + PHOTO_STRIPES_PER_BATCH;
    if(end > PHOTO_STRIPE_COUNT) {
        end = PHOTO_STRIPE_COUNT;
    }

    for(int32_t i = s_photo_apply_index; i < end; i++) {
        lv_image_set_src(s_photo_stripes[i], NULL);
        lv_image_set_src(s_photo_stripes[i], &s_photo_dsc[i]);
        lv_obj_invalidate(s_photo_stripes[i]);
    }

    s_photo_apply_index = end;
    if(s_photo_apply_index >= PHOTO_STRIPE_COUNT) {
        s_photo_apply_pending = false;
        if(s_photo_placeholder_label != NULL) {
            lv_obj_add_flag(s_photo_placeholder_label, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, "photo loaded");
    }
}

static bool consume_pending_photo_memory(void)
{
    bool pending = false;

    portENTER_CRITICAL(&s_value_lock);
    pending = s_photo_memory_pending;
    s_photo_memory_pending = false;
    portEXIT_CRITICAL(&s_value_lock);

    return pending;
}

static void switch_panel(int32_t delta)
{
    if(s_panel_build_pending) {
        return;
    }

    int32_t next = (s_active_panel + delta) % SYS_DASHBOARD_PANEL_COUNT;
    if(next < 0) {
        next += SYS_DASHBOARD_PANEL_COUNT;
    }

    if(next == s_active_panel) {
        return;
    }

    bool leaving_photo = photo_panel_active();
    begin_panel_transition();
    lv_obj_t * old_screen = lv_screen_active();
    s_active_panel = next;
    s_panel_build_pending = true;
    s_panel_black_frame_pending = true;
    s_panel_transition_hold_frames = PANEL_TRANSITION_HOLD_FRAMES;
    reset_dashboard_face_refs();
    reset_photo_page_refs();
    if(leaving_photo) {
        release_photo_buffer();
    }
    s_panel_staging_screen = create_panel_screen();
    lv_screen_load(s_panel_staging_screen);
    if(old_screen != NULL && old_screen != s_panel_staging_screen) {
        lv_obj_delete(old_screen);
    }
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(PANEL_TRANSITION_BLACK_DELAY_MS));
    if(!bsp_display_lock(500)) {
        return;
    }
    ESP_LOGI(TAG, "panel switched to %s (%" LV_PRId32 ")",
             active_panel_name(), s_active_panel);
}

static void show_panel(int32_t panel_index)
{
    if(s_panel_build_pending) {
        return;
    }

    if(panel_index < 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT ||
       panel_index == s_active_panel) {
        return;
    }

    bool leaving_photo = photo_panel_active();
    begin_panel_transition();
    lv_obj_t * old_screen = lv_screen_active();
    s_active_panel = panel_index;
    s_panel_build_pending = true;
    s_panel_black_frame_pending = true;
    s_panel_transition_hold_frames = PANEL_TRANSITION_HOLD_FRAMES;
    reset_dashboard_face_refs();
    reset_photo_page_refs();
    if(leaving_photo) {
        release_photo_buffer();
    }
    s_panel_staging_screen = create_panel_screen();
    lv_screen_load(s_panel_staging_screen);
    if(old_screen != NULL && old_screen != s_panel_staging_screen) {
        lv_obj_delete(old_screen);
    }
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(PANEL_TRANSITION_BLACK_DELAY_MS));
    if(!bsp_display_lock(500)) {
        return;
    }
    ESP_LOGI(TAG, "panel switched to %s (%" LV_PRId32 ")",
             active_panel_name(), s_active_panel);
}

static void apply_pending_panel_request(void)
{
    if(s_panel_build_pending) {
        return;
    }

    int32_t panel_index = -1;
    int32_t panel_delta = 0;

    portENTER_CRITICAL(&s_value_lock);
    panel_index = s_pending_panel_index;
    panel_delta = s_pending_panel_delta;
    s_pending_panel_index = -1;
    s_pending_panel_delta = 0;
    portEXIT_CRITICAL(&s_value_lock);

    if(panel_index >= 0) {
        show_panel(panel_index);
    }
    else if(panel_delta != 0) {
        switch_panel(panel_delta);
    }
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
        portENTER_CRITICAL(&s_value_lock);
        s_pending_panel_delta++;
        portEXIT_CRITICAL(&s_value_lock);
    }
    else if(dir == LV_DIR_RIGHT) {
        portENTER_CRITICAL(&s_value_lock);
        s_pending_panel_delta--;
        portEXIT_CRITICAL(&s_value_lock);
    }
}

static void photo_click_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    if(!photo_panel_active() || s_config.photo_click_cb == NULL) {
        return;
    }

    s_config.photo_click_cb(s_config.photo_click_user_ctx);
}

static void dashboard_update_task(void * arg)
{
    LV_UNUSED(arg);

    uint32_t frame = 0;
    int64_t last_sample_us = 0;
    ESP_LOGI(TAG, "dashboard update task started");

    while(1) {
        bool memory_photo_ready = consume_pending_photo_memory();

        if(bsp_display_lock(500)) {
            int64_t now_us = esp_timer_get_time();

            apply_pending_panel_request();
            finish_pending_panel_build();
            if(photo_panel_active() && memory_photo_ready) {
                apply_staged_photo();
            }
            if(photo_panel_active() && !s_panel_build_pending) {
                apply_photo_stripe_batch();
            }

            if(!s_panel_build_pending && now_us - last_sample_us >= (int64_t)data_refresh_ms() * 1000) {
                last_sample_us = now_us;
                if(photo_panel_active()) {
                    update_photo_labels();
                }
                else {
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
            }

            if(!photo_panel_active() && !s_panel_build_pending) {
                for(int i = 0; i < SYS_DASHBOARD_METRIC_COUNT; i++) {
                    int32_t delta = s_metrics[i].target - s_metrics[i].arc_value;
                    int32_t step = delta / 8;

                    if(step == 0 && delta != 0) {
                        step = delta > 0 ? 1 : -1;
                    }

                    set_metric_arc_value(&s_metrics[i], s_metrics[i].arc_value + step);
                }

                if((frame % DASHBOARD_SWEEP_FRAME_DIVIDER) == 0) {
                    for(int i = 0; i < 3; i++) {
                        lv_arc_set_rotation(s_bg_sweep[i], (int32_t)((frame * (2 + i)) + (i * 120)) % 360);
                    }
                    lv_arc_set_rotation(s_bg_sweep[3], (int32_t)((frame * 5) + 180) % 360);
                }
            }

            bsp_display_unlock();
        }
        else {
            int64_t now_us = esp_timer_get_time();
            if(!photo_panel_active() || (now_us - s_last_lvgl_lock_timeout_log_us) >= 2000000) {
                s_last_lvgl_lock_timeout_log_us = now_us;
                ESP_LOGW(TAG, "LVGL lock timeout");
            }
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

static void enable_gesture_bubble(lv_obj_t * obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void configure_gesture_input(void)
{
    lv_indev_t * indev = bsp_display_get_input_dev();
    if(indev == NULL) {
        return;
    }

    lv_indev_set_gesture_min_distance(indev, 28);
    lv_indev_set_gesture_min_velocity(indev, 3);
}

static lv_obj_t * create_bottom_mask(lv_obj_t * parent)
{
    lv_obj_t * mask = lv_arc_create(parent);
    enable_gesture_bubble(mask);
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
    enable_gesture_bubble(label);
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
    enable_gesture_bubble(metric->arc);
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
    enable_gesture_bubble(arc);
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
    enable_gesture_bubble(wrap);
    lv_obj_set_size(wrap, HISTORY_WRAP_W, HISTORY_BAR_H);
    style_plain(wrap);
    lv_obj_align(wrap, LV_ALIGN_BOTTOM_MID, 0, y);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wrap, HISTORY_BAR_GAP, LV_PART_MAIN);

    for(int i = 0; i < HISTORY_COUNT; i++) {
        metric->bars[i] = lv_bar_create(wrap);
        enable_gesture_bubble(metric->bars[i]);
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

static lv_obj_t * create_photo_overlay_label(lv_obj_t * parent, const char * text,
                                             const lv_font_t * font, lv_align_t align,
                                             int32_t x, int32_t y)
{
    lv_obj_t * label = create_label(parent, text, font, lv_color_hex(0xffffff), align, x, y);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_outline_stroke_width(label, 2, LV_PART_MAIN);
    lv_obj_set_style_text_outline_stroke_color(label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_outline_stroke_opa(label, LV_OPA_80, LV_PART_MAIN);
    return label;
}

static void create_photo_page(lv_obj_t * screen)
{
    s_photo_page = lv_obj_create(screen);
    enable_gesture_bubble(s_photo_page);
    lv_obj_set_size(s_photo_page, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_photo_page);
    lv_obj_set_style_radius(s_photo_page, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_photo_page, false, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_photo_page, lv_color_hex(0x080a0c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_photo_page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_photo_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_photo_page, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_photo_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_photo_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_photo_page, dashboard_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_photo_page, photo_click_cb, LV_EVENT_SHORT_CLICKED, NULL);

    for(int i = 0; i < PHOTO_STRIPE_COUNT; i++) {
        int32_t stripe_y = i * PHOTO_STRIPE_HEIGHT;
        int32_t stripe_h = PHOTO_STRIPE_HEIGHT;
        if((stripe_y + stripe_h) > SCREEN_SIZE) {
            stripe_h = SCREEN_SIZE - stripe_y;
        }

        s_photo_stripes[i] = lv_image_create(s_photo_page);
        enable_gesture_bubble(s_photo_stripes[i]);
        lv_obj_add_flag(s_photo_stripes[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(s_photo_stripes[i], SCREEN_SIZE, stripe_h);
        lv_obj_align(s_photo_stripes[i], LV_ALIGN_TOP_MID, 0, stripe_y);
        lv_obj_add_event_cb(s_photo_stripes[i], photo_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    }

    s_photo_placeholder_label = create_label(s_photo_page, "", &lv_font_montserrat_18,
                                             lv_color_hex(0x9da5af), LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_background(s_photo_placeholder_label);

    s_photo_battery_label = create_photo_overlay_label(s_photo_page, "--%", &lv_font_montserrat_14,
                                                       LV_ALIGN_CENTER, 0, PHOTO_BATTERY_Y);
    s_photo_time_label = create_photo_overlay_label(s_photo_page, "--:--", &lv_font_montserrat_26,
                                                    LV_ALIGN_CENTER, 0, PHOTO_TIME_Y);
    s_photo_date_label = create_photo_overlay_label(s_photo_page, "---- -- --", &lv_font_montserrat_26,
                                                    LV_ALIGN_CENTER, 0, PHOTO_DATE_Y);
    s_photo_weather_label = create_photo_overlay_label(s_photo_page, "--", &lv_font_montserrat_26,
                                                       LV_ALIGN_CENTER, 0, PHOTO_WEATHER_Y);

    if(s_photo_buf == NULL && s_photo_fallback_data != NULL && s_photo_fallback_len == PHOTO_IMAGE_BYTES) {
        apply_staged_photo();
    }
    update_photo_labels();
}

static void create_dashboard_face(lv_obj_t * screen)
{
    lv_obj_t * face = lv_obj_create(screen);
    s_face = face;
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

    s_battery_label = create_label(face, "", &lv_font_montserrat_14,
                                   lv_color_hex(0xf5f7fb), LV_ALIGN_TOP_MID, 0, TOP_BATTERY_Y);
    update_battery_label();

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

static void create_dashboard(void)
{
    configure_gesture_input();
    lv_obj_t * screen = create_panel_screen();
    lv_screen_load(screen);
    populate_panel_screen(screen);
}

void sys_dashboard_start(const sys_dashboard_config_t * config)
{
    if(config == NULL) {
        ESP_LOGE(TAG, "missing dashboard config");
        return;
    }

    memcpy(&s_config, config, sizeof(s_config));
    if(s_config.brand_name == NULL) s_config.brand_name = "";
    if(s_config.weather_text == NULL) s_config.weather_text = "--";
    snprintf(s_weather_text, sizeof(s_weather_text), "%s", s_config.weather_text);
    int32_t initial_temp_c = 0;
    if(parse_weather_temperature(s_config.weather_text, &initial_temp_c)) {
        s_weather_temperature_c = initial_temp_c;
        s_weather_temperature_valid = true;
    }
    if(s_config.default_panel_index < 0 || s_config.default_panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
        s_config.default_panel_index = 0;
    }
    s_battery_percent = clamp_value(s_config.battery_percent, -1, 100);
    s_clock_cpu_history_value = s_config.metrics[0].value;
    s_active_panel = s_config.default_panel_index;
    if(SYS_DASHBOARD_PANEL_COUNT > 0 && s_config.panel_names[0] == NULL) s_config.panel_names[0] = "FnOS";
    if(SYS_DASHBOARD_PANEL_COUNT > 1 && s_config.panel_names[1] == NULL) s_config.panel_names[1] = "Windows11";
    if(SYS_DASHBOARD_PANEL_COUNT > 2 && s_config.panel_names[2] == NULL) s_config.panel_names[2] = "Photo";
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
    if(panel_index < 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT ||
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
    if(panel_index < 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
        return;
    }

    portENTER_CRITICAL(&s_value_lock);
    s_tx_external_value[panel_index] = value;
    s_tx_external_valid[panel_index] = true;
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_set_panel_rx_value(int32_t panel_index, int32_t value)
{
    if(panel_index < 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
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

void sys_dashboard_set_battery_percent(int32_t percent)
{
    percent = clamp_value(percent, -1, 100);
    portENTER_CRITICAL(&s_value_lock);
    s_battery_percent = percent;
    portEXIT_CRITICAL(&s_value_lock);

    if(bsp_display_lock(100)) {
        update_battery_label();
        update_photo_labels();
        bsp_display_unlock();
    }
}

void sys_dashboard_next_panel(void)
{
    portENTER_CRITICAL(&s_value_lock);
    s_pending_panel_delta++;
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_show_panel(int32_t panel_index)
{
    if(panel_index < 0 || panel_index >= SYS_DASHBOARD_PANEL_COUNT) {
        return;
    }

    portENTER_CRITICAL(&s_value_lock);
    s_pending_panel_index = panel_index;
    s_pending_panel_delta = 0;
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_set_photo_path(const char * path)
{
    LV_UNUSED(path);
}

void sys_dashboard_reload_photo(void)
{
}

void sys_dashboard_set_photo_buffer(const uint8_t * data, size_t len)
{
    if(data == NULL || len != PHOTO_IMAGE_BYTES) {
        return;
    }

    if(!photo_panel_active()) {
        return;
    }

    if(s_photo_buf == NULL) {
        s_photo_buf = heap_caps_malloc(PHOTO_IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(s_photo_buf == NULL) {
            ESP_LOGE(TAG, "failed to allocate PSRAM photo buffer: %u bytes", (unsigned)PHOTO_IMAGE_BYTES);
            return;
        }
    }

    memcpy(s_photo_buf, data, PHOTO_IMAGE_BYTES);

    portENTER_CRITICAL(&s_value_lock);
    s_photo_memory_pending = true;
    portEXIT_CRITICAL(&s_value_lock);
}

void sys_dashboard_set_fallback_photo(const uint8_t * data, size_t len)
{
    if(data == NULL || len != PHOTO_IMAGE_BYTES) {
        return;
    }

    s_photo_fallback_data = data;
    s_photo_fallback_len = len;
}

void sys_dashboard_set_metric_value(int32_t index, int32_t value)
{
    sys_dashboard_set_panel_metric_value(0, index, value);
}

void sys_dashboard_set_tx_value(int32_t value)
{
    sys_dashboard_set_panel_tx_value(0, value);
}

void sys_dashboard_set_rx_value(int32_t value)
{
    sys_dashboard_set_panel_rx_value(0, value);
}
