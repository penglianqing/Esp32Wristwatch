idf_component_register(
    SRCS "wifi_time.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash log freertos)
