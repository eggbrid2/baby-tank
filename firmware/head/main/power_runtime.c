#include "power_runtime.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "chassis_bridge.h"

#define HEAD_BATTERY_ADC_UNIT ADC_UNIT_2
#define HEAD_BATTERY_ADC_CHANNEL ADC_CHANNEL_3
#define HEAD_BATTERY_DIVIDER 2
#define POWER_SAMPLE_INTERVAL_MS 5000U
#define CHASSIS_SAMPLE_INTERVAL_MS 60000U

static const char *TAG = "baby_tank_power";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_calibration;
static TaskHandle_t s_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_head_percent = -1;
static int s_chassis_percent = -1;
static int s_filtered_head_mv;

static uint8_t percent_from_millivolts(int millivolts)
{
    /* A bounded, piecewise Li-ion estimate is enough for the low-battery
     * threshold. It avoids presenting voltage-derived values as fuel-gauge
     * precision while behaving better than a single linear slope. */
    static const struct { int mv; uint8_t percent; } points[] = {
        {3300, 0}, {3500, 10}, {3650, 20}, {3750, 40},
        {3850, 60}, {4000, 80}, {4200, 100},
    };
    if (millivolts <= points[0].mv) return 0U;
    for (size_t index = 1; index < sizeof(points) / sizeof(points[0]); ++index) {
        if (millivolts <= points[index].mv) {
            int span_mv = points[index].mv - points[index - 1U].mv;
            int span_percent = points[index].percent - points[index - 1U].percent;
            return (uint8_t)(points[index - 1U].percent +
                (millivolts - points[index - 1U].mv) * span_percent / span_mv);
        }
    }
    return 100U;
}

static void power_task(void *argument)
{
    (void)argument;
    TickType_t next_chassis_sample = 0U;
    bool head_error_logged = false;
    for (;;) {
        int raw = 0;
        int adc_mv = 0;
        esp_err_t result = adc_oneshot_read(s_adc, HEAD_BATTERY_ADC_CHANNEL,
                                            &raw);
        if (result == ESP_OK) {
            result = adc_cali_raw_to_voltage(s_calibration, raw, &adc_mv);
        }
        if (result == ESP_OK) {
            int battery_mv = adc_mv * HEAD_BATTERY_DIVIDER;
            s_filtered_head_mv = s_filtered_head_mv == 0 ? battery_mv :
                (s_filtered_head_mv * 7 + battery_mv) / 8;
            uint8_t percent = percent_from_millivolts(s_filtered_head_mv);
            portENTER_CRITICAL(&s_lock);
            s_head_percent = percent;
            portEXIT_CRITICAL(&s_lock);
            head_error_logged = false;
        } else if (!head_error_logged) {
            ESP_LOGW(TAG, "head battery ADC unavailable: %s",
                     esp_err_to_name(result));
            head_error_logged = true;
        }

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_chassis_sample) >= 0 &&
            baby_tank_chassis_bridge_link_verified()) {
            uint8_t percent = 0U;
            result = baby_tank_chassis_bridge_get_battery_percent(&percent);
            if (result == ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                s_chassis_percent = percent;
                portEXIT_CRITICAL(&s_lock);
            } else if (result != ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGD(TAG, "chassis battery query unavailable: %s",
                         esp_err_to_name(result));
            }
            next_chassis_sample = now + pdMS_TO_TICKS(CHASSIS_SAMPLE_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t baby_tank_power_runtime_start(void)
{
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "power runtime already started");
    const adc_oneshot_unit_init_cfg_t unit = {
        .unit_id = HEAD_BATTERY_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG,
                        "head battery ADC init failed");
    const adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(
        s_adc, HEAD_BATTERY_ADC_CHANNEL, &channel), TAG,
        "head battery ADC channel failed");
    const adc_cali_curve_fitting_config_t calibration = {
        .unit_id = HEAD_BATTERY_ADC_UNIT,
        .chan = HEAD_BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(
        &calibration, &s_calibration), TAG,
        "head battery ADC calibration failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(power_task, "baby_tank_power", 3072,
                                    NULL, 4, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "power task allocation failed");
    ESP_LOGI(TAG, "dual battery monitor ready; chassis requires bNN protocol");
    return ESP_OK;
}

bool baby_tank_power_head_percent(uint8_t *percent)
{
    if (percent == NULL) return false;
    portENTER_CRITICAL(&s_lock);
    int value = s_head_percent;
    portEXIT_CRITICAL(&s_lock);
    if (value < 0 || value > 100) return false;
    *percent = (uint8_t)value;
    return true;
}

bool baby_tank_power_chassis_percent(uint8_t *percent)
{
    if (percent == NULL) return false;
    portENTER_CRITICAL(&s_lock);
    int value = s_chassis_percent;
    portEXIT_CRITICAL(&s_lock);
    if (value < 0 || value > 100) return false;
    *percent = (uint8_t)value;
    return true;
}
