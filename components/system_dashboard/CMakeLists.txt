idf_component_register(
    SRCS "system_dashboard.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl esp_timer log freertos esp32_s3_touch_amoled_1_75c)
