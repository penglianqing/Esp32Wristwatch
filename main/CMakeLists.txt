idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES system_dashboard wifi_time ha_mqtt power_monitor esp_timer lvgl__lvgl esp32_s3_touch_amoled_1_75c)
