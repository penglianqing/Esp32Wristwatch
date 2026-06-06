#include "power_monitor.h"

#include <cstdlib>
#include <cstring>
#include <inttypes.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static const char * TAG = "power_monitor";
static constexpr uint32_t I2C_TIMEOUT_MS = 1000;
static constexpr uint32_t SAMPLE_PERIOD_MS = 5000;

static XPowersPMU s_pmu;
static i2c_master_dev_handle_t s_pmu_dev;
static power_monitor_battery_cb_t s_battery_cb;
static void * s_battery_user_ctx;
static power_monitor_power_button_cb_t s_power_button_cb;
static void * s_power_button_user_ctx;
static TaskHandle_t s_task_handle;
static bool s_ready;

static int pmu_register_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t * data, uint8_t len)
{
    (void)dev_addr;
    esp_err_t ret = i2c_master_transmit_receive(s_pmu_dev, &reg_addr, 1, data, len, I2C_TIMEOUT_MS);
    return ret == ESP_OK ? 0 : -1;
}

static int pmu_register_write_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t * data, uint8_t len)
{
    (void)dev_addr;

    uint8_t * buffer = static_cast<uint8_t *>(std::malloc((size_t)len + 1));
    if(buffer == nullptr) {
        return -1;
    }

    buffer[0] = reg_addr;
    std::memcpy(&buffer[1], data, len);
    esp_err_t ret = i2c_master_transmit(s_pmu_dev, buffer, len + 1, I2C_TIMEOUT_MS);
    std::free(buffer);
    return ret == ESP_OK ? 0 : -1;
}

static int32_t read_battery_percent(void)
{
    if(!s_ready || !s_pmu.isBatteryConnect()) {
        return -1;
    }

    int percent = s_pmu.getBatteryPercent();
    if(percent < 0) return -1;
    if(percent > 100) return 100;
    return percent;
}

static void power_monitor_task(void * arg)
{
    (void)arg;
    int64_t last_battery_sample_ms = -(int64_t)SAMPLE_PERIOD_MS;

    while(true) {
        s_pmu.getIrqStatus();
        if(s_pmu.isPekeyShortPressIrq() && s_power_button_cb != nullptr) {
            s_power_button_cb(s_power_button_user_ctx);
        }
        s_pmu.clearIrqStatus();

        int64_t now_ms = esp_timer_get_time() / 1000;
        if(now_ms - last_battery_sample_ms >= SAMPLE_PERIOD_MS) {
            last_battery_sample_ms = now_ms;
            int32_t percent = read_battery_percent();
            if(s_battery_cb != nullptr) {
                s_battery_cb(percent, s_battery_user_ctx);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t init_pmu(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if(bus == nullptr) {
        ESP_LOGE(TAG, "failed to get BSP I2C bus");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = AXP2101_SLAVE_ADDRESS;
    dev_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;

    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_config, &s_pmu_dev);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to add AXP2101 I2C device: %s", esp_err_to_name(ret));
        return ret;
    }

    if(!s_pmu.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte)) {
        ESP_LOGE(TAG, "AXP2101 is not online");
        return ESP_FAIL;
    }

    s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    s_pmu.clearIrqStatus();
    s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    s_pmu.enableBattDetection();
    s_pmu.enableBattVoltageMeasure();
    s_pmu.enableVbusVoltageMeasure();
    s_pmu.enableSystemVoltageMeasure();
    s_pmu.enableTemperatureMeasure();
    s_pmu.disableTSPinMeasure();
    s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    s_ready = true;
    ESP_LOGI(TAG, "AXP2101 battery monitor ready, percent=%" PRId32, read_battery_percent());
    return ESP_OK;
}

extern "C" esp_err_t power_monitor_start(power_monitor_battery_cb_t battery_cb, void * user_ctx)
{
    if(s_task_handle != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    s_battery_cb = battery_cb;
    s_battery_user_ctx = user_ctx;

    esp_err_t ret = init_pmu();
    if(ret != ESP_OK) {
        return ret;
    }

    if(s_battery_cb != nullptr) {
        s_battery_cb(read_battery_percent(), s_battery_user_ctx);
    }

    BaseType_t task_ok = xTaskCreate(power_monitor_task, "power_monitor", 4096, nullptr, 6, &s_task_handle);
    if(task_ok != pdPASS) {
        s_task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

extern "C" int32_t power_monitor_get_battery_percent(void)
{
    return read_battery_percent();
}

extern "C" void power_monitor_set_power_button_cb(power_monitor_power_button_cb_t power_button_cb, void * user_ctx)
{
    s_power_button_cb = power_button_cb;
    s_power_button_user_ctx = user_ctx;
}
