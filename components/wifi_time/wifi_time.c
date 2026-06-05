#include "wifi_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_RETRY_MAX     8

static const char * TAG = "wifi_time";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count;
static bool s_started;
static bool s_connected;
static bool s_time_synced;

static uint32_t default_timeout(uint32_t value, uint32_t fallback)
{
    return value ? value : fallback;
}

static void wifi_event_handler(void * arg, esp_event_base_t event_base,
                               int32_t event_id, void * event_data)
{
    (void)arg;

    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "wifi sta started, connecting...");
        esp_wifi_connect();
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t * disconnected = event_data;
        s_connected = false;
        ESP_LOGW(TAG, "wifi disconnected, reason=%d", disconnected ? disconnected->reason : -1);
        if(s_retry_count < WIFI_RETRY_MAX) {
            s_retry_count++;
            esp_wifi_connect();
            ESP_LOGI(TAG, "retry wifi connection %d/%d", s_retry_count, WIFI_RETRY_MAX);
        }
        else {
            ESP_LOGW(TAG, "wifi retry limit reached");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t * got_ip = event_data;
        s_retry_count = 0;
        s_connected = true;
        ESP_LOGI(TAG, "wifi got ip: " IPSTR, IP2STR(&got_ip->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        ret = nvs_flash_init();
    }

    return ret;
}

esp_err_t wifi_time_start(const wifi_time_config_t * config)
{
    if(config == NULL || config->ssid == NULL || config->ssid[0] == '\0') {
        ESP_LOGW(TAG, "wifi ssid is empty, skip wifi time sync");
        return ESP_ERR_INVALID_ARG;
    }

    if(s_started) {
        ESP_LOGI(TAG, "wifi time already started");
        return ESP_OK;
    }

    const char * password = config->password ? config->password : "";
    const char * timezone = config->timezone ? config->timezone : "CST-8";
    const char * sntp_server = config->sntp_server ? config->sntp_server : "pool.ntp.org";
    uint32_t connect_timeout_ms = default_timeout(config->connect_timeout_ms, 15000);
    uint32_t sync_timeout_ms = default_timeout(config->sync_timeout_ms, 15000);

    ESP_LOGI(TAG, "init wifi time, ssid=%s, timezone=%s, sntp=%s",
             config->ssid, timezone, sntp_server);
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "init nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "init netif");

    esp_err_t loop_ret = esp_event_loop_create_default();
    if(loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(loop_ret, TAG, "create event loop");
    }

    s_wifi_event_group = xEventGroupCreate();
    if(s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), TAG, "init wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL), TAG, "ip handler");

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", config->ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", password);
    wifi_config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set wifi config");
    ESP_LOGI(TAG, "starting wifi, connect timeout=%" PRIu32 "ms", connect_timeout_ms);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(connect_timeout_ms));

    if((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "wifi connect timeout or failed, bits=0x%lx", (unsigned long)bits);
        s_started = true;
        return ESP_FAIL;
    }

    setenv("TZ", timezone, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to %s", timezone);

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(sntp_server);
    ESP_LOGI(TAG, "starting sntp sync, timeout=%" PRIu32 "ms", sync_timeout_ms);
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp_config), TAG, "init sntp");

    esp_err_t sync_ret = esp_netif_sntp_sync_wait(
        pdMS_TO_TICKS(sync_timeout_ms));

    s_time_synced = (sync_ret == ESP_OK);
    s_started = true;

    if(sync_ret == ESP_OK) {
        time_t now = 0;
        struct tm timeinfo = {0};
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "time synced by %s: %04d-%02d-%02d %02d:%02d:%02d",
                 sntp_server,
                 timeinfo.tm_year + 1900,
                 timeinfo.tm_mon + 1,
                 timeinfo.tm_mday,
                 timeinfo.tm_hour,
                 timeinfo.tm_min,
                 timeinfo.tm_sec);
    }
    else {
        ESP_LOGW(TAG, "sntp sync not ready yet: %s", esp_err_to_name(sync_ret));
    }

    return sync_ret;
}

bool wifi_time_is_connected(void)
{
    return s_connected;
}

bool wifi_time_is_synced(void)
{
    return s_time_synced;
}
