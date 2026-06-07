#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "ha_mqtt.h"
#include "image_fetcher.h"
#include "power_monitor.h"
#include "system_dashboard.h"
#include "wifi_time.h"

#ifndef CONFIG_DASHBOARD_WIFI_SSID
#define CONFIG_DASHBOARD_WIFI_SSID ""
#endif

#ifndef CONFIG_DASHBOARD_WIFI_PASSWORD
#define CONFIG_DASHBOARD_WIFI_PASSWORD ""
#endif

#ifndef CONFIG_DASHBOARD_HOSTNAME
#define CONFIG_DASHBOARD_HOSTNAME "fnos-dashboard"
#endif

#ifndef CONFIG_DASHBOARD_STATIC_IP
#define CONFIG_DASHBOARD_STATIC_IP "192.168.1.222"
#endif

#ifndef CONFIG_DASHBOARD_GATEWAY
#define CONFIG_DASHBOARD_GATEWAY "192.168.1.1"
#endif

#ifndef CONFIG_DASHBOARD_NETMASK
#define CONFIG_DASHBOARD_NETMASK "255.255.255.0"
#endif

#ifndef CONFIG_DASHBOARD_DNS
#define CONFIG_DASHBOARD_DNS "192.168.1.1"
#endif

#ifndef CONFIG_DASHBOARD_TIMEZONE
#define CONFIG_DASHBOARD_TIMEZONE "CST-8"
#endif

#ifndef CONFIG_DASHBOARD_SNTP_SERVER
#define CONFIG_DASHBOARD_SNTP_SERVER "pool.ntp.org"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_URI
#define CONFIG_DASHBOARD_MQTT_URI "mqtt://192.168.1.214:1883"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_USERNAME
#define CONFIG_DASHBOARD_MQTT_USERNAME "esp32"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_PASSWORD
#define CONFIG_DASHBOARD_MQTT_PASSWORD "passwd"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_CLIENT_ID
#define CONFIG_DASHBOARD_MQTT_CLIENT_ID "fnos-dashboard"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_FNOS_CPU_TOPIC
#define CONFIG_DASHBOARD_MQTT_FNOS_CPU_TOPIC "esp32/fnos/cpu/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_FNOS_MEM_TOPIC
#define CONFIG_DASHBOARD_MQTT_FNOS_MEM_TOPIC "esp32/fnos/mem/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_FNOS_GPU_TOPIC
#define CONFIG_DASHBOARD_MQTT_FNOS_GPU_TOPIC "esp32/fnos/gpu/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_FNOS_TX_TOPIC
#define CONFIG_DASHBOARD_MQTT_FNOS_TX_TOPIC "esp32/fnos/tx/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_FNOS_RX_TOPIC
#define CONFIG_DASHBOARD_MQTT_FNOS_RX_TOPIC "esp32/fnos/rx/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_WIN_CPU_TOPIC
#define CONFIG_DASHBOARD_MQTT_WIN_CPU_TOPIC "esp32/windows11/cpu/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_WIN_MEM_TOPIC
#define CONFIG_DASHBOARD_MQTT_WIN_MEM_TOPIC "esp32/windows11/mem/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_WIN_GPU_TOPIC
#define CONFIG_DASHBOARD_MQTT_WIN_GPU_TOPIC "esp32/windows11/gpu/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_WIN_TX_TOPIC
#define CONFIG_DASHBOARD_MQTT_WIN_TX_TOPIC "esp32/windows11/tx/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_WIN_RX_TOPIC
#define CONFIG_DASHBOARD_MQTT_WIN_RX_TOPIC "esp32/windows11/rx/state"
#endif

#ifndef CONFIG_DASHBOARD_MQTT_WEATHER_TOPIC
#define CONFIG_DASHBOARD_MQTT_WEATHER_TOPIC "esp32/dial/weather/state"
#endif

#ifndef CONFIG_DASHBOARD_PHOTO_URL
#define CONFIG_DASHBOARD_PHOTO_URL "http://192.168.1.212:8000/image.rgb565"
#endif

#ifndef CONFIG_DASHBOARD_PHOTO_REFRESH_MS
#define CONFIG_DASHBOARD_PHOTO_REFRESH_MS 300000
#endif

static const char * TAG = "main";
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BOOT_BUTTON_POLL_MS 20
#define BOOT_BUTTON_DEBOUNCE_MS 60
#define BOOT_BUTTON_TASK_STACK 4096
#define MQTT_START_DELAY_MS 20000
#define PHOTO_IMAGE_SIZE 466
#define PHOTO_IMAGE_BYTES ((PHOTO_IMAGE_SIZE) * (PHOTO_IMAGE_SIZE) * 2)
#define PHOTO_REFRESH_CLICK_COOLDOWN_MS 1500

static bool s_photo_fetcher_started;
static int64_t s_last_photo_click_request_us;

extern const uint8_t photo_fallback_rgb565_start[] asm("_binary_photo_fallback_rgb565_start");
extern const uint8_t photo_fallback_rgb565_end[] asm("_binary_photo_fallback_rgb565_end");

static void dashboard_time_text(char * buf, size_t buf_size, void * user_ctx)
{
    (void)user_ctx;

    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);

    if(now < 1700000000 || localtime_r(&now, &timeinfo) == NULL) {
        snprintf(buf, buf_size, "--:--");
        return;
    }

    strftime(buf, buf_size, "%H:%M:%S", &timeinfo);
}

static void dashboard_battery_update(int32_t percent, void * user_ctx)
{
    (void)user_ctx;
    sys_dashboard_set_battery_percent(percent);
}

static void dashboard_power_button_press(void * user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "power button short press");
    sys_dashboard_show_panel(2);
}

static void dashboard_photo_updated(void * user_ctx)
{
    (void)user_ctx;
}

static void dashboard_photo_data(const uint8_t * data, size_t len, void * user_ctx)
{
    (void)user_ctx;
    sys_dashboard_set_photo_buffer(data, len);
}

static esp_err_t start_photo_fetcher(void)
{
    if(s_photo_fetcher_started) {
        return ESP_OK;
    }

    const image_fetcher_config_t image_fetcher = {
        .url = CONFIG_DASHBOARD_PHOTO_URL,
        .expected_size = PHOTO_IMAGE_BYTES,
        .refresh_ms = CONFIG_DASHBOARD_PHOTO_REFRESH_MS,
        .update_cb = dashboard_photo_updated,
        .data_cb = dashboard_photo_data,
    };
    esp_err_t ret = image_fetcher_start(&image_fetcher);
    if(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        s_photo_fetcher_started = true;
        ESP_LOGI(TAG, "photo fetcher ready: %s", CONFIG_DASHBOARD_PHOTO_URL);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "photo fetcher start failed: %s", esp_err_to_name(ret));
    return ret;
}

static void dashboard_photo_click(void * user_ctx)
{
    (void)user_ctx;
    int64_t now_us = esp_timer_get_time();
    if((now_us - s_last_photo_click_request_us) < (PHOTO_REFRESH_CLICK_COOLDOWN_MS * 1000LL)) {
        ESP_LOGI(TAG, "photo refresh skipped: cooldown");
        return;
    }
    s_last_photo_click_request_us = now_us;
    ESP_LOGI(TAG, "photo page refresh requested");
    esp_err_t start_ret = start_photo_fetcher();
    if(start_ret != ESP_OK) {
        ESP_LOGW(TAG, "photo update skipped: %s", esp_err_to_name(start_ret));
        return;
    }

    esp_err_t ret = image_fetcher_request_update();
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "photo update request failed: %s", esp_err_to_name(ret));
    }
}

static esp_err_t __attribute__((unused)) start_mqtt_client(void)
{
    const ha_mqtt_config_t ha_mqtt = {
        .uri = CONFIG_DASHBOARD_MQTT_URI,
        .username = CONFIG_DASHBOARD_MQTT_USERNAME,
        .password = CONFIG_DASHBOARD_MQTT_PASSWORD,
        .client_id = CONFIG_DASHBOARD_MQTT_CLIENT_ID,
        .fnos_cpu_topic = CONFIG_DASHBOARD_MQTT_FNOS_CPU_TOPIC,
        .fnos_mem_topic = CONFIG_DASHBOARD_MQTT_FNOS_MEM_TOPIC,
        .fnos_gpu_topic = CONFIG_DASHBOARD_MQTT_FNOS_GPU_TOPIC,
        .fnos_tx_topic = CONFIG_DASHBOARD_MQTT_FNOS_TX_TOPIC,
        .fnos_rx_topic = CONFIG_DASHBOARD_MQTT_FNOS_RX_TOPIC,
        .win_cpu_topic = CONFIG_DASHBOARD_MQTT_WIN_CPU_TOPIC,
        .win_mem_topic = CONFIG_DASHBOARD_MQTT_WIN_MEM_TOPIC,
        .win_gpu_topic = CONFIG_DASHBOARD_MQTT_WIN_GPU_TOPIC,
        .win_tx_topic = CONFIG_DASHBOARD_MQTT_WIN_TX_TOPIC,
        .win_rx_topic = CONFIG_DASHBOARD_MQTT_WIN_RX_TOPIC,
        .weather_topic = CONFIG_DASHBOARD_MQTT_WEATHER_TOPIC,
    };

    esp_err_t ret = ha_mqtt_start(&ha_mqtt);
    if(ret == ESP_OK) {
        ESP_LOGI(TAG, "mqtt ready");
        return ESP_OK;
    }
    if(ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "mqtt already running");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "mqtt init not ready: %s", esp_err_to_name(ret));
    return ret;
}

static void __attribute__((unused)) delayed_mqtt_task(void * arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(MQTT_START_DELAY_MS));
    ESP_LOGI(TAG, "starting delayed mqtt");
    start_mqtt_client();
    vTaskDelete(NULL);
}

static void boot_button_task(void * arg)
{
    (void)arg;
    bool was_pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
    uint32_t stable_ticks = 0;

    while(1) {
        bool pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;

        if(pressed == was_pressed) {
            stable_ticks += BOOT_BUTTON_POLL_MS;
        }
        else {
            stable_ticks = 0;
            was_pressed = pressed;
        }

        if(pressed && stable_ticks == BOOT_BUTTON_DEBOUNCE_MS) {
            sys_dashboard_next_panel();
        }

        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));
    }
}

static esp_err_t boot_button_start(void)
{
    const gpio_config_t boot_button = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&boot_button);
    if(ret != ESP_OK) {
        return ret;
    }

    BaseType_t task_ok = xTaskCreate(boot_button_task, "boot_button", BOOT_BUTTON_TASK_STACK, NULL, 5, NULL);
    return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_main(void)
{
    lv_rand_set_seed((uint32_t)esp_timer_get_time());

    const wifi_time_config_t wifi_time = {
        .ssid = CONFIG_DASHBOARD_WIFI_SSID,
        .password = CONFIG_DASHBOARD_WIFI_PASSWORD,
        .hostname = CONFIG_DASHBOARD_HOSTNAME,
        .static_ip = CONFIG_DASHBOARD_STATIC_IP,
        .gateway = CONFIG_DASHBOARD_GATEWAY,
        .netmask = CONFIG_DASHBOARD_NETMASK,
        .dns = CONFIG_DASHBOARD_DNS,
        .timezone = CONFIG_DASHBOARD_TIMEZONE,
        .sntp_server = CONFIG_DASHBOARD_SNTP_SERVER,
        .connect_timeout_ms = 15000,
        .sync_timeout_ms = 15000,
    };

    ESP_LOGI(TAG, "wifi config: ssid=%s, ip=%s, timezone=%s, sntp=%s",
             wifi_time.ssid, wifi_time.static_ip, wifi_time.timezone, wifi_time.sntp_server);
    esp_err_t time_ret = wifi_time_start(&wifi_time);
    if(time_ret != ESP_OK && time_ret != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "wifi time init not ready: %s", esp_err_to_name(time_ret));
    }

    bsp_display_start();
    sys_dashboard_set_fallback_photo(photo_fallback_rgb565_start,
                                     (size_t)(photo_fallback_rgb565_end - photo_fallback_rgb565_start));

    const sys_dashboard_config_t dashboard = {
        .data_refresh_ms = 1000,
        .frame_refresh_hz = 30,
        .brand_name = "FnOS",
        .panel_names = {"FnOS", "Windows11", "Photo"},
        .default_panel_index = 2,
        .battery_percent = -1,
        .weather_text = "--",
        .time_text = "--:--",
        .time_cb = dashboard_time_text,
        .photo_click_cb = dashboard_photo_click,
        .history_metric_index = 0,
        .metrics = {
            {
                .name = "CPU",
                .unit = "%",
                .value = 0,
                .color = lv_color_hex(0x19d6b4),
                .ring_size = 408,
                .ring_width = 16,
                .mock_min = 8,
                .mock_max = 94,
                .value_font = &lv_font_montserrat_26,
            },
            {
                .name = "MEM",
                .unit = "%",
                .value = 0,
                .color = lv_color_hex(0xffc857),
                .ring_size = 326,
                .ring_width = 14,
                .mock_min = 8,
                .mock_max = 94,
                .value_font = &lv_font_montserrat_26,
            },
            {
                .name = "GPU",
                .unit = "%",
                .value = 0,
                .color = lv_color_hex(0xff4fa3),
                .ring_size = 248,
                .ring_width = 12,
                .mock_min = 8,
                .mock_max = 94,
                .value_font = &lv_font_montserrat_24,
            },
        },
        .tx = {
            .name = "TX",
            .unit = "Mbps",
            .value = 0,
            .mock_min = 40,
            .mock_max = 180,
        },
        .rx = {
            .name = "RX",
            .unit = "Mbps",
            .value = 0,
            .mock_min = 120,
            .mock_max = 900,
        },
    };

    sys_dashboard_start(&dashboard);

    BaseType_t mqtt_task_ok = xTaskCreate(delayed_mqtt_task, "mqtt_delayed", 4096, NULL, 4, NULL);
    if(mqtt_task_ok != pdPASS) {
        ESP_LOGW(TAG, "failed to create delayed mqtt task");
    }

    esp_err_t button_ret = boot_button_start();
    if(button_ret != ESP_OK) {
        ESP_LOGW(TAG, "boot button init not ready: %s", esp_err_to_name(button_ret));
    }

    power_monitor_set_power_button_cb(dashboard_power_button_press, NULL);
    esp_err_t power_ret = power_monitor_start(dashboard_battery_update, NULL);
    if(power_ret != ESP_OK && power_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "power monitor init not ready: %s", esp_err_to_name(power_ret));
    }
}
