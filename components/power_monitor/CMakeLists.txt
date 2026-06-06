idf_component_register(
    SRCS "power_monitor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp32_s3_touch_amoled_1_75c XPowersLib freertos log)
