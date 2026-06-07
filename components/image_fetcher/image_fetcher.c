#include "image_fetcher.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char * TAG = "image_fetcher";
/* Keep the HTTP receive buffer modest. The photo payload itself lives in PSRAM,
 * but the client buffer and task stack still compete for precious internal RAM
 * with the SPI/LVGL DMA path. */
static const size_t DOWNLOAD_BUFFER_SIZE = 4096;
static const int DOWNLOAD_TIMEOUT_MS = 60000;
static image_fetcher_config_t s_config;
static TaskHandle_t s_task_handle;
static portMUX_TYPE s_fetch_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_fetch_in_progress;
static bool s_fetch_requested;

static bool has_text(const char * text)
{
    return text != NULL && text[0] != '\0';
}

static esp_err_t download_image(void)
{
    if(!has_text(s_config.url)) {
        ESP_LOGW(TAG, "photo GET skipped: invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "photo GET start: %s", s_config.url);
    esp_http_client_config_t http_cfg = {
        .url = s_config.url,
        .timeout_ms = DOWNLOAD_TIMEOUT_MS,
        .buffer_size = DOWNLOAD_BUFFER_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if(client == NULL) {
        return ESP_FAIL;
    }

    uint8_t * buffer = heap_caps_malloc(DOWNLOAD_BUFFER_SIZE, MALLOC_CAP_8BIT);
    uint8_t * image_data = NULL;
    if(buffer == NULL) {
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "failed to allocate download buffer: %u", (unsigned)DOWNLOAD_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = esp_http_client_open(client, 0);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "open %s failed: %s", s_config.url, esp_err_to_name(ret));
        goto done;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "photo GET response: status=%d content_length=%d expected=%u",
             status, content_length, (unsigned)s_config.expected_size);
    if(status != 200) {
        ESP_LOGW(TAG, "unexpected photo http status: %d", status);
        ret = ESP_FAIL;
        goto done;
    }

    if(s_config.expected_size > 0 && content_length > 0 && (size_t)content_length != s_config.expected_size) {
        ESP_LOGW(TAG, "unexpected photo content length: %d", content_length);
        ret = ESP_FAIL;
        goto done;
    }

    size_t total = 0;
    size_t target_size = s_config.expected_size;
    int64_t download_start_us = esp_timer_get_time();
    int64_t read_total_us = 0;
    int64_t copy_total_us = 0;
    if(target_size == 0 && content_length > 0) {
        target_size = (size_t)content_length;
    }
    if(target_size > 0) {
        image_data = heap_caps_malloc(target_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(image_data == NULL) {
            ret = ESP_ERR_NO_MEM;
            ESP_LOGW(TAG, "failed to allocate image buffer: %u", (unsigned)target_size);
            goto done;
        }
    }

    while(1) {
        int need = (int)DOWNLOAD_BUFFER_SIZE;
        if(target_size > 0) {
            size_t remaining = target_size - total;
            if(remaining == 0) {
                break;
            }
            if(remaining < (size_t)need) {
                need = (int)remaining;
            }
        }

        int64_t read_start_us = esp_timer_get_time();
        int read_len = esp_http_client_read(client, (char *)buffer, need);
        read_total_us += esp_timer_get_time() - read_start_us;
        if(read_len < 0) {
            ESP_LOGW(TAG, "photo GET read failed after %u bytes: read_len=%d err=%s",
                     (unsigned)total, read_len, read_len < 0 ? esp_err_to_name(read_len) : "EOF");
            ret = ESP_FAIL;
            break;
        }
        if(read_len == 0) {
            if(target_size == 0 || total == target_size) {
                break;
            }
            ESP_LOGW(TAG, "photo GET ended early at %u bytes", (unsigned)total);
            ret = ESP_FAIL;
            break;
        }

        if(image_data != NULL) {
            int64_t copy_start_us = esp_timer_get_time();
            memcpy(image_data + total, buffer, read_len);
            copy_total_us += esp_timer_get_time() - copy_start_us;
        }
        total += read_len;
    }

    if(ret == ESP_OK && (target_size == 0 || total == target_size)) {
        ESP_LOGI(TAG, "photo GET ready: %u bytes", (unsigned)total);
        int64_t total_us = esp_timer_get_time() - download_start_us;
        ESP_LOGI(TAG, "photo GET timing total=%lldms read=%lldms copy=%lldms",
                 total_us / 1000, read_total_us / 1000, copy_total_us / 1000);
        ESP_LOGI(TAG, "after photo GET heap internal=%u dma=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if(s_config.data_cb != NULL) {
            s_config.data_cb(image_data, total, s_config.user_ctx);
        }
        if(s_config.update_cb != NULL) {
            s_config.update_cb(s_config.user_ctx);
        }
    }
    else {
        ESP_LOGW(TAG, "photo download incomplete: %u/%u",
                 (unsigned)total, (unsigned)target_size);
        ret = ESP_FAIL;
    }

done:
    heap_caps_free(buffer);
    if(image_data != NULL) {
        heap_caps_free(image_data);
    }
    esp_http_client_cleanup(client);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "photo GET failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void image_fetcher_task(void * arg)
{
    (void)arg;
    ESP_LOGI(TAG, "image fetcher task started");
    bool started = false;

    while(1) {
        uint32_t refresh_ms = s_config.refresh_ms ? s_config.refresh_ms : 300000;
        TickType_t wait_ticks = started ? pdMS_TO_TICKS(refresh_ms) : portMAX_DELAY;
        uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);
        bool should_fetch = false;

        portENTER_CRITICAL(&s_fetch_lock);
        if(s_fetch_requested) {
            should_fetch = true;
            s_fetch_requested = false;
        }
        else if(started && notified == 0) {
            should_fetch = true;
        }
        s_fetch_in_progress = should_fetch;
        portEXIT_CRITICAL(&s_fetch_lock);

        if(!should_fetch) {
            continue;
        }

        download_image();

        portENTER_CRITICAL(&s_fetch_lock);
        s_fetch_in_progress = false;
        portEXIT_CRITICAL(&s_fetch_lock);
        started = true;
    }
}

esp_err_t image_fetcher_start(const image_fetcher_config_t * config)
{
    if(config == NULL || s_task_handle != NULL) {
        ESP_LOGW(TAG, "image fetcher start rejected: config=%p task=%p", config, s_task_handle);
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&s_config, config, sizeof(s_config));
    BaseType_t task_ok = xTaskCreate(image_fetcher_task, "image_fetch", 4096, NULL, 5, &s_task_handle);
    if(task_ok != pdPASS) {
        s_task_handle = NULL;
        ESP_LOGW(TAG, "image fetcher task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "image fetcher task created");

    return ESP_OK;
}

esp_err_t image_fetcher_request_update(void)
{
    if(s_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_fetch_lock);
    bool already_pending = s_fetch_requested;
    bool busy = s_fetch_in_progress;
    s_fetch_requested = true;
    portEXIT_CRITICAL(&s_fetch_lock);

    if(already_pending || busy) {
        ESP_LOGI(TAG, "photo GET request coalesced");
    }
    else {
        ESP_LOGI(TAG, "photo GET requested");
    }
    xTaskNotifyGive(s_task_handle);
    return ESP_OK;
}
