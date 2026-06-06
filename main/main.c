#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "ha_mqtt.h"
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

static const char * TAG = "main";

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

    const sys_dashboard_config_t dashboard = {
        .data_refresh_ms = 1000,
        .frame_refresh_hz = 60,
        .brand_name = "Time",
        .panel_names = {"Time", "FnOS", "Windows11"},
        .default_panel_index = 0,
        .weather_text = "Beijing --",
        .time_text = "--:--",
        .time_cb = dashboard_time_text,
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

    esp_err_t mqtt_ret = ha_mqtt_start(&ha_mqtt);
    if(mqtt_ret != ESP_OK && mqtt_ret != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "mqtt init not ready: %s", esp_err_to_name(mqtt_ret));
    }
}
