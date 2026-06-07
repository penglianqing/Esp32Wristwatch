#include "ha_mqtt.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "system_dashboard.h"

static const char * TAG = "ha_mqtt";

typedef enum {
    HA_VALUE_CPU,
    HA_VALUE_MEM,
    HA_VALUE_GPU,
    HA_VALUE_TX,
    HA_VALUE_RX,
} ha_value_id_t;

typedef struct {
    const char * topic;
    int32_t panel_index;
    ha_value_id_t id;
    const char * name;
} ha_topic_map_t;

static ha_mqtt_config_t s_config;
static ha_topic_map_t s_topics[10];
static esp_mqtt_client_handle_t s_client;

static bool has_text(const char * text)
{
    return text != NULL && text[0] != '\0';
}

static bool topic_matches(const esp_mqtt_event_handle_t event, const char * topic)
{
    return has_text(topic) &&
           event->topic_len == (int)strlen(topic) &&
           memcmp(event->topic, topic, event->topic_len) == 0;
}

static bool parse_value(const esp_mqtt_event_handle_t event, int32_t * out_value)
{
    char buf[24];
    size_t len = event->data_len < (int)(sizeof(buf) - 1) ? (size_t)event->data_len : sizeof(buf) - 1;
    memcpy(buf, event->data, len);
    buf[len] = '\0';

    char * end = NULL;
    float value = strtof(buf, &end);
    if(end == buf) {
        return false;
    }

    *out_value = (int32_t)(value + (value >= 0 ? 0.5f : -0.5f));
    return true;
}

static void handle_weather(const esp_mqtt_event_handle_t event)
{
    char buf[64];
    size_t len = event->data_len < (int)(sizeof(buf) - 1) ? (size_t)event->data_len : sizeof(buf) - 1;
    memcpy(buf, event->data, len);
    buf[len] = '\0';
    sys_dashboard_set_weather_text(buf);

    int32_t temp_c = 0;
    bool has_temp = false;
    for(char * p = buf; *p != '\0'; p++) {
        if((*p >= '0' && *p <= '9') ||
           ((*p == '-' || *p == '+') && p[1] >= '0' && p[1] <= '9')) {
            char * end = NULL;
            float value = strtof(p, &end);
            if(end != p) {
                temp_c = (int32_t)(value + (value >= 0 ? 0.5f : -0.5f));
                has_temp = true;
                p = end - 1;
            }
        }
    }

    if(has_temp) {
        sys_dashboard_set_weather_temperature(temp_c);
        ESP_LOGI(TAG, "mqtt weather=%s temp=%" PRId32 "C", buf, temp_c);
    }
    else {
        ESP_LOGI(TAG, "mqtt weather=%s", buf);
    }
}

static const char * value_name(ha_value_id_t id)
{
    switch(id) {
    case HA_VALUE_CPU:
        return "CPU";
    case HA_VALUE_MEM:
        return "MEM";
    case HA_VALUE_GPU:
        return "GPU";
    case HA_VALUE_TX:
        return "TX";
    case HA_VALUE_RX:
        return "RX";
    }

    return "?";
}

static void apply_value(int32_t panel_index, ha_value_id_t id, int32_t value)
{
    switch(id) {
    case HA_VALUE_CPU:
        sys_dashboard_set_panel_metric_value(panel_index, 0, value);
        break;
    case HA_VALUE_MEM:
        sys_dashboard_set_panel_metric_value(panel_index, 1, value);
        break;
    case HA_VALUE_GPU:
        sys_dashboard_set_panel_metric_value(panel_index, 2, value);
        break;
    case HA_VALUE_TX:
        sys_dashboard_set_panel_tx_value(panel_index, value);
        break;
    case HA_VALUE_RX:
        sys_dashboard_set_panel_rx_value(panel_index, value);
        break;
    }
}

static void subscribe_topics(esp_mqtt_client_handle_t client)
{
    for(size_t i = 0; i < sizeof(s_topics) / sizeof(s_topics[0]); i++) {
        if(has_text(s_topics[i].topic)) {
            int msg_id = esp_mqtt_client_subscribe(client, s_topics[i].topic, 0);
            ESP_LOGI(TAG, "mqtt subscribe %s topic=%s msg_id=%d",
                     s_topics[i].name, s_topics[i].topic, msg_id);
        }
    }

    if(has_text(s_config.weather_topic)) {
        int msg_id = esp_mqtt_client_subscribe(client, s_config.weather_topic, 0);
        ESP_LOGI(TAG, "mqtt subscribe Weather topic=%s msg_id=%d", s_config.weather_topic, msg_id);
    }
}

static void handle_data(const esp_mqtt_event_handle_t event)
{
    int32_t value = 0;
    ESP_LOGI(TAG, "mqtt data topic=%.*s payload=%.*s",
             event->topic_len, event->topic, event->data_len, event->data);

    if(topic_matches(event, s_config.weather_topic)) {
        handle_weather(event);
        return;
    }

    if(!parse_value(event, &value)) {
        ESP_LOGW(TAG, "ignore non numeric mqtt payload topic=%.*s payload=%.*s",
                 event->topic_len, event->topic, event->data_len, event->data);
        return;
    }

    for(size_t i = 0; i < sizeof(s_topics) / sizeof(s_topics[0]); i++) {
        if(topic_matches(event, s_topics[i].topic)) {
            apply_value(s_topics[i].panel_index, s_topics[i].id, value);
            ESP_LOGI(TAG, "mqtt sensor panel=%" PRId32 " %s=%" PRId32,
                     s_topics[i].panel_index, value_name(s_topics[i].id), value);
            return;
        }
    }

    ESP_LOGW(TAG, "mqtt topic not mapped: %.*s", event->topic_len, event->topic);
}

static void mqtt_event_handler(void * handler_args, esp_event_base_t base,
                               int32_t event_id, void * event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = event_data;
    switch((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected to %s", s_config.uri);
        subscribe_topics(event->client);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "mqtt disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_data(event);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error type=%d", event->error_handle ? event->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

esp_err_t ha_mqtt_start(const ha_mqtt_config_t * config)
{
    if(config == NULL || !has_text(config->uri)) {
        ESP_LOGW(TAG, "mqtt uri is empty, keep dashboard mock data");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(s_config));
    s_topics[0] = (ha_topic_map_t){s_config.fnos_cpu_topic, 0, HA_VALUE_CPU, "FnOS CPU"};
    s_topics[1] = (ha_topic_map_t){s_config.fnos_mem_topic, 0, HA_VALUE_MEM, "FnOS MEM"};
    s_topics[2] = (ha_topic_map_t){s_config.fnos_gpu_topic, 0, HA_VALUE_GPU, "FnOS GPU"};
    s_topics[3] = (ha_topic_map_t){s_config.fnos_tx_topic, 0, HA_VALUE_TX, "FnOS TX"};
    s_topics[4] = (ha_topic_map_t){s_config.fnos_rx_topic, 0, HA_VALUE_RX, "FnOS RX"};
    s_topics[5] = (ha_topic_map_t){s_config.win_cpu_topic, 1, HA_VALUE_CPU, "Windows11 CPU"};
    s_topics[6] = (ha_topic_map_t){s_config.win_mem_topic, 1, HA_VALUE_MEM, "Windows11 MEM"};
    s_topics[7] = (ha_topic_map_t){s_config.win_gpu_topic, 1, HA_VALUE_GPU, "Windows11 GPU"};
    s_topics[8] = (ha_topic_map_t){s_config.win_tx_topic, 1, HA_VALUE_TX, "Windows11 TX"};
    s_topics[9] = (ha_topic_map_t){s_config.win_rx_topic, 1, HA_VALUE_RX, "Windows11 RX"};

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = s_config.uri,
        .credentials.username = has_text(s_config.username) ? s_config.username : NULL,
        .credentials.client_id = has_text(s_config.client_id) ? s_config.client_id : NULL,
        .credentials.authentication.password = has_text(s_config.password) ? s_config.password : NULL,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 1024,
        .buffer.out_size = 512,
        .task.stack_size = 4096,
    };

    s_client = esp_mqtt_client_init(&mqtt_config);
    if(s_client == NULL) {
        ESP_LOGE(TAG, "failed to create mqtt client");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_LOGI(TAG, "starting mqtt uri=%s client_id=%s username=%s",
             s_config.uri,
             has_text(s_config.client_id) ? s_config.client_id : "(auto)",
             has_text(s_config.username) ? s_config.username : "(none)");
    for(size_t i = 0; i < sizeof(s_topics) / sizeof(s_topics[0]); i++) {
        ESP_LOGI(TAG, "mqtt topic map %s -> %s", s_topics[i].name, s_topics[i].topic);
    }
    if(has_text(s_config.weather_topic)) {
        ESP_LOGI(TAG, "mqtt topic map Weather -> %s", s_config.weather_topic);
    }
    return esp_mqtt_client_start(s_client);
}
