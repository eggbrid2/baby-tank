#include "touch_wake.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/touch_sens.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TOUCH_WAKE_CHANNEL_COUNT 3
#define TOUCH_WAKE_INITIAL_SCANS 3
#define TOUCH_WAKE_SCAN_TIMEOUT_MS 2000
#define TOUCH_WAKE_DEBOUNCE_MS 1200
#define TOUCH_TOP_CONFIRM_MS 80
#define TOUCH_SIDE_CONFIRM_MS 120
#define TOUCH_SIDE_SECOND_SAMPLE_MS 60
#define TOUCH_SIDE_MIN_DELTA_PER_MILLE 100U
#define TOUCH_WAKE_TASK_STACK 3072
#define TOUCH_WAKE_TASK_PRIORITY 6

typedef struct {
    int channel_id;
    baby_tank_touch_zone_t zone;
    uint32_t threshold_per_mille;
    const char *name;
} touch_zone_config_t;

static const touch_zone_config_t TOUCH_ZONES[TOUCH_WAKE_CHANNEL_COUNT] = {
    {.channel_id = 1, .zone = BABY_TANK_TOUCH_TOP,
     .threshold_per_mille = 25U, .name = "top"},
    {.channel_id = 2, .zone = BABY_TANK_TOUCH_LEFT,
     .threshold_per_mille = 100U, .name = "left"},
    {.channel_id = 3, .zone = BABY_TANK_TOUCH_RIGHT,
     .threshold_per_mille = 100U, .name = "right"},
};

static const char *TAG = "baby_tank_touch";

static touch_sensor_handle_t s_sensor;
static touch_channel_handle_t s_channels[TOUCH_WAKE_CHANNEL_COUNT];
static TaskHandle_t s_task;
static baby_tank_touch_wake_callback_t s_callback;
static void *s_callback_context;
static volatile uint32_t s_active_latch;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static baby_tank_touch_snapshot_t s_snapshot;

static void update_touch_snapshot(void)
{
    baby_tank_touch_snapshot_t next;
    portENTER_CRITICAL(&s_snapshot_lock);
    next = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
    next.active_mask = 0U;
    for (size_t index = 0; index < TOUCH_WAKE_CHANNEL_COUNT; ++index) {
        const touch_zone_config_t *zone = &TOUCH_ZONES[index];
        if ((s_active_latch & (1UL << zone->channel_id)) != 0U) {
            next.active_mask |= (uint8_t)(1U << index);
        }
        uint32_t smooth[TOUCH_SAMPLE_CFG_NUM] = {0};
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = {0};
        if (touch_channel_read_data(s_channels[index],
                                    TOUCH_CHAN_DATA_TYPE_SMOOTH,
                                    smooth) == ESP_OK &&
            touch_channel_read_data(s_channels[index],
                                    TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                    benchmark) == ESP_OK &&
            benchmark[0] > 0U) {
            /* On this board a held touch raises the smooth reading above
             * baseline (the same polarity observed on the top pad). */
            uint32_t delta = smooth[0] > benchmark[0]
                                 ? smooth[0] - benchmark[0] : 0U;
            uint32_t per_mille = delta * 1000U / benchmark[0];
            next.delta_per_mille[index] =
                (uint16_t)(per_mille > UINT16_MAX ? UINT16_MAX : per_mille);
        }
    }
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = next;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void log_top_touch_sample(const char *reason)
{
    uint32_t smooth[TOUCH_SAMPLE_CFG_NUM] = {0};
    uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = {0};
    if (touch_channel_read_data(s_channels[0], TOUCH_CHAN_DATA_TYPE_SMOOTH,
                                smooth) != ESP_OK ||
        touch_channel_read_data(s_channels[0], TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                benchmark) != ESP_OK) return;
    ESP_LOGI(TAG, "top touch sample %s smooth=%lu baseline=%lu delta=%ld active=%u",
             reason, (unsigned long)smooth[0],
             (unsigned long)benchmark[0],
             (long)benchmark[0] - (long)smooth[0],
             (unsigned)((s_active_latch & (1UL << 1)) != 0U));
}

static bool IRAM_ATTR touch_on_active(
    touch_sensor_handle_t sensor,
    const touch_active_event_data_t *event,
    void *context)
{
    (void)sensor;
    (void)context;
    if (event == NULL || event->chan_id < 1 || event->chan_id > 3 ||
        s_task == NULL) {
        return false;
    }
    const uint32_t bit = 1UL << event->chan_id;
    if ((s_active_latch & bit) != 0U) return false;
    s_active_latch |= bit;
    BaseType_t task_woken = pdFALSE;
    xTaskNotifyFromISR(s_task, bit, eSetBits, &task_woken);
    return task_woken == pdTRUE;
}

static bool IRAM_ATTR touch_on_inactive(
    touch_sensor_handle_t sensor,
    const touch_inactive_event_data_t *event,
    void *context)
{
    (void)sensor;
    (void)context;
    if (event != NULL && event->chan_id >= 1 && event->chan_id <= 3) {
        s_active_latch &= ~(1UL << event->chan_id);
    }
    return false;
}

static void touch_wake_task(void *argument)
{
    (void)argument;
    TickType_t last_wake[TOUCH_WAKE_CHANNEL_COUNT] = {0};
    TickType_t last_log = 0;
    while (true) {
        uint32_t active_channels = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &active_channels,
                            pdMS_TO_TICKS(250)) != pdTRUE) {
            update_touch_snapshot();
            TickType_t now = xTaskGetTickCount();
            if (last_log == 0 || now - last_log >= pdMS_TO_TICKS(5000)) {
                log_top_touch_sample("idle");
                last_log = now;
            }
            continue;
        }
        for (size_t index = 0; index < TOUCH_WAKE_CHANNEL_COUNT; ++index) {
            const touch_zone_config_t *zone = &TOUCH_ZONES[index];
            if ((active_channels & (1UL << zone->channel_id)) == 0) {
                continue;
            }
            if (zone->zone == BABY_TANK_TOUCH_TOP) {
                /* A single low-threshold sample caused unintended
                 * barge-ins while ASR was uploading. Require a held touch. */
                vTaskDelay(pdMS_TO_TICKS(TOUCH_TOP_CONFIRM_MS));
                if ((s_active_latch & (1UL << zone->channel_id)) == 0U) {
                    continue;
                }
                log_top_touch_sample("held");
            } else {
                /* Side pads have shown chassis-coupled false edges. Require
                 * sustained contact and two above-threshold readings before
                 * triggering local audio; rejected edges remain observable. */
                bool confirmed = true;
                for (uint8_t sample = 0U; sample < 2U; ++sample) {
                    vTaskDelay(pdMS_TO_TICKS(sample == 0U
                        ? TOUCH_SIDE_CONFIRM_MS : TOUCH_SIDE_SECOND_SAMPLE_MS));
                    update_touch_snapshot();
                    uint16_t delta;
                    portENTER_CRITICAL(&s_snapshot_lock);
                    delta = s_snapshot.delta_per_mille[index];
                    portEXIT_CRITICAL(&s_snapshot_lock);
                    if ((s_active_latch & (1UL << zone->channel_id)) == 0U ||
                        delta < TOUCH_SIDE_MIN_DELTA_PER_MILLE) {
                        ESP_LOGI(TAG, "%s touch rejected sample=%u delta=%u active=%u",
                                 zone->name, (unsigned)sample, (unsigned)delta,
                                 (unsigned)((s_active_latch &
                                     (1UL << zone->channel_id)) != 0U));
                        confirmed = false;
                        break;
                    }
                }
                if (!confirmed) continue;
            }
            TickType_t now = xTaskGetTickCount();
            if (last_wake[index] != 0 &&
                now - last_wake[index] <
                    pdMS_TO_TICKS(TOUCH_WAKE_DEBOUNCE_MS)) {
                continue;
            }
            last_wake[index] = now;
            portENTER_CRITICAL(&s_snapshot_lock);
            ++s_snapshot.trigger_count[index];
            portEXIT_CRITICAL(&s_snapshot_lock);
            update_touch_snapshot();
            ESP_LOGI(TAG, "%s touch wake channel=%d", zone->name,
                     zone->channel_id);
            s_callback(zone->zone, s_callback_context);
        }
    }
}

esp_err_t baby_tank_touch_wake_start(
    baby_tank_touch_wake_callback_t callback,
    void *context)
{
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "wake callback is required");
    ESP_RETURN_ON_FALSE(s_sensor == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "touch wake already started");

    static touch_sensor_sample_config_t sample_config[] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(
            500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2),
    };
    touch_sensor_config_t sensor_config =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample_config);
    ESP_RETURN_ON_ERROR(touch_sensor_new_controller(&sensor_config, &s_sensor),
                        TAG, "unable to create touch controller");

    touch_channel_config_t channel_configs[TOUCH_WAKE_CHANNEL_COUNT];
    for (size_t index = 0; index < TOUCH_WAKE_CHANNEL_COUNT; ++index) {
        channel_configs[index] = (touch_channel_config_t) {
            .active_thresh = {2000},
            .charge_speed = TOUCH_CHARGE_SPEED_7,
            .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        };
        ESP_RETURN_ON_ERROR(
            touch_sensor_new_channel(s_sensor, TOUCH_ZONES[index].channel_id,
                                     &channel_configs[index],
                                     &s_channels[index]),
            TAG, "unable to create touch channel");
    }

    touch_sensor_filter_config_t filter_config =
        TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_RETURN_ON_ERROR(touch_sensor_config_filter(s_sensor, &filter_config),
                        TAG, "unable to configure touch filter");

    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_sensor), TAG,
                        "unable to enable touch calibration");
    for (int scan = 0; scan < TOUCH_WAKE_INITIAL_SCANS; ++scan) {
        ESP_RETURN_ON_ERROR(
            touch_sensor_trigger_oneshot_scanning(
                s_sensor, TOUCH_WAKE_SCAN_TIMEOUT_MS),
            TAG, "touch calibration scan failed");
    }
    ESP_RETURN_ON_ERROR(touch_sensor_disable(s_sensor), TAG,
                        "unable to finish touch calibration");

    uint32_t benchmarks[TOUCH_WAKE_CHANNEL_COUNT] = {0};
    for (size_t index = 0; index < TOUCH_WAKE_CHANNEL_COUNT; ++index) {
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = {0};
        ESP_RETURN_ON_ERROR(
            touch_channel_read_data(s_channels[index],
                                    TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                    benchmark),
            TAG, "unable to read touch benchmark");
        benchmarks[index] = benchmark[0];
        channel_configs[index].active_thresh[0] =
            benchmark[0] * TOUCH_ZONES[index].threshold_per_mille / 1000U;
        ESP_RETURN_ON_FALSE(channel_configs[index].active_thresh[0] > 0U,
                            ESP_ERR_INVALID_STATE, TAG,
                            "invalid touch benchmark");
        ESP_RETURN_ON_ERROR(
            touch_sensor_reconfig_channel(s_channels[index],
                                          &channel_configs[index]),
            TAG, "unable to set calibrated touch threshold");
    }

    s_callback = callback;
    s_callback_context = context;
    BaseType_t created = xTaskCreate(touch_wake_task, "touch_wake",
                                     TOUCH_WAKE_TASK_STACK, NULL,
                                     TOUCH_WAKE_TASK_PRIORITY, &s_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "unable to create touch wake task");

    const touch_event_callbacks_t callbacks = {
        .on_active = touch_on_active,
        .on_inactive = touch_on_inactive,
    };
    ESP_RETURN_ON_ERROR(
        touch_sensor_register_callbacks(s_sensor, &callbacks, NULL), TAG,
        "unable to register touch callback");
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_sensor), TAG,
                        "unable to enable touch sensor");
    ESP_RETURN_ON_ERROR(touch_sensor_start_continuous_scanning(s_sensor), TAG,
                        "unable to start touch scanning");

    for (size_t index = 0; index < TOUCH_WAKE_CHANNEL_COUNT; ++index) {
        touch_chan_info_t channel_info = {0};
        ESP_RETURN_ON_ERROR(
            touch_sensor_get_channel_info(s_channels[index], &channel_info),
            TAG, "unable to read touch channel info");
        ESP_LOGI(TAG,
                 "%s touch ready channel=%d gpio=%d benchmark=%lu threshold=%lu",
                 TOUCH_ZONES[index].name, TOUCH_ZONES[index].channel_id,
                 channel_info.chan_gpio, (unsigned long)benchmarks[index],
                 (unsigned long)channel_configs[index].active_thresh[0]);
        portENTER_CRITICAL(&s_snapshot_lock);
        s_snapshot.threshold_per_mille[index] =
            (uint16_t)TOUCH_ZONES[index].threshold_per_mille;
        portEXIT_CRITICAL(&s_snapshot_lock);
    }
    update_touch_snapshot();
    return ESP_OK;
}

bool baby_tank_touch_wake_ready(void)
{
    return s_sensor != NULL && s_task != NULL;
}

esp_err_t baby_tank_touch_wake_get_snapshot(
    baby_tank_touch_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "touch snapshot output is required");
    ESP_RETURN_ON_FALSE(baby_tank_touch_wake_ready(), ESP_ERR_INVALID_STATE,
                        TAG, "touch wake is not ready");
    portENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return ESP_OK;
}
