idf_component_register(
    SRCS "image_fetcher.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_timer freertos log heap)
