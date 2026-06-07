#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*image_fetcher_update_cb_t)(void * user_ctx);
typedef void (*image_fetcher_data_cb_t)(const uint8_t * data, size_t len, void * user_ctx);

typedef struct {
    const char * url;
    size_t expected_size;
    uint32_t refresh_ms;
    image_fetcher_update_cb_t update_cb;
    image_fetcher_data_cb_t data_cb;
    void * user_ctx;
} image_fetcher_config_t;

esp_err_t image_fetcher_start(const image_fetcher_config_t * config);
esp_err_t image_fetcher_request_update(void);

#ifdef __cplusplus
}
#endif
