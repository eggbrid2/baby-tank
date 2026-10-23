#include "chassis_bridge.h"
#include "runtime_policy.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* The installed tracked_chassis image accepts x/y text on UART1. These are
 * the head board's wiring pins, not the chassis controller's pin numbers. */
#define CHASSIS_UART UART_NUM_1
#define CHASSIS_TX GPIO_NUM_38
#define CHASSIS_RX GPIO_NUM_48
#define CHASSIS_BAUD 115200
#define CHASSIS_MAX_PER_MILLE 1000
#define CHASSIS_DEADMAN_US 300000
#define CHASSIS_SEND_PERIOD_MS 80
#define CHASSIS_ECHO_TIMEOUT_MS 60
/* This chassis consistently arcs right with equal open-loop PWM, meaning the
 * left track travels slightly farther. The vendor chassis firmware exposes
 * calibration in 2.5% steps; apply the equivalent trim in the head so it is
 * deterministic across chassis/head reboot order. */
#define CHASSIS_STRAIGHT_LEFT_GAIN_PER_MILLE 975

static const char *TAG = "baby_tank_chassis";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_tx_lock;
static bool s_ready;
static bool s_link_verified;
static bool s_active;
static int16_t s_left;
static int16_t s_right;
static int64_t s_updated_us;
static uint32_t s_control_epoch;

static esp_err_t send_command(const char *command, int length)
{
    size_t stale_bytes = 0U;
    if (uart_get_buffered_data_len(CHASSIS_UART, &stale_bytes) == ESP_OK &&
        stale_bytes > 0U) {
        uart_flush_input(CHASSIS_UART);
        ESP_LOGW(TAG, "discarded %zu stale chassis UART bytes before command",
                 stale_bytes);
    }
    int sent = uart_write_bytes(CHASSIS_UART, command, length);
    if (sent != length) {
        ESP_LOGW(TAG, "incomplete chassis UART write: %d/%d bytes", sent, length);
        return ESP_FAIL;
    }

    char echo[48];
    int received = 0;
    int64_t deadline_us = esp_timer_get_time() + CHASSIS_ECHO_TIMEOUT_MS * 1000;
    while (received < length) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) break;
        TickType_t wait_ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        if (wait_ticks == 0) wait_ticks = 1;
        int count = uart_read_bytes(CHASSIS_UART, echo + received,
                                    sizeof(echo) - received, wait_ticks);
        if (count <= 0) break;
        received += count;
    }
    if (received == length && memcmp(echo, command, length) == 0) {
        return ESP_OK;
    }
    if (received > 0) {
        s_link_verified = false;
        uart_flush_input(CHASSIS_UART);
        ESP_LOGW(TAG, "chassis UART echo mismatch: expected=%d received=%d",
                 length, received);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_link_verified = false;
    uart_flush_input(CHASSIS_UART);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_motion(int16_t left, int16_t right)
{
    if (left != 0 && left == right) {
        left = (int16_t)((int32_t)left *
                         CHASSIS_STRAIGHT_LEFT_GAIN_PER_MILLE / 1000);
    }
    /* Chassis firmware maps x=turn and y=forward. A trailing newline keeps
     * each UART read parseable even when packets arrive close together. */
    float x = (float)(left - right) / 2000.0f;
    float y = (float)(left + right) / 2000.0f;
    char command[32];
    int length = snprintf(command, sizeof(command), "x%.3f y%.3f\n", x, y);
    if (length <= 0 || length >= (int)sizeof(command)) return ESP_FAIL;
    esp_err_t result = send_command(command, length);
    if (left == 0 && right == 0 && result != ESP_OK) {
        /* Retrying STOP is safe even if the first command reached the motor;
         * never retry a motion command after an uncertain echo. */
        vTaskDelay(pdMS_TO_TICKS(10));
        result = send_command(command, length);
    }
    return result;
}

static void chassis_sender(void *argument)
{
    (void)argument;
    bool last_motion = false;
    for (;;) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        bool active;
        int16_t left;
        int16_t right;
        int64_t updated;
        portENTER_CRITICAL(&s_lock);
        active = s_active;
        left = s_left;
        right = s_right;
        updated = s_updated_us;
        portEXIT_CRITICAL(&s_lock);
        if (active && esp_timer_get_time() - updated < CHASSIS_DEADMAN_US) {
            esp_err_t result = send_motion(left, right);
            if (result == ESP_OK) {
                last_motion = true;
            } else {
                portENTER_CRITICAL(&s_lock);
                s_active = false;
                portEXIT_CRITICAL(&s_lock);
                ++s_control_epoch;
                ESP_LOGE(TAG, "motion echo lost; emergency stop");
                send_motion(0, 0);
                last_motion = false;
            }
        } else if (last_motion) {
            if (active) {
                portENTER_CRITICAL(&s_lock);
                s_active = false;
                portEXIT_CRITICAL(&s_lock);
                ++s_control_epoch;
                ESP_LOGW(TAG, "motion command expired; emergency stop");
            }
            send_motion(0, 0);
            last_motion = false;
        }
        xSemaphoreGive(s_tx_lock);
        vTaskDelay(pdMS_TO_TICKS(CHASSIS_SEND_PERIOD_MS));
    }
}

esp_err_t baby_tank_chassis_bridge_start(void)
{
    if (s_ready) return ESP_OK;
    const uart_config_t config = {
        .baud_rate = CHASSIS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t result = uart_driver_install(CHASSIS_UART, 256, 0, 0, NULL, 0);
    if (result != ESP_OK) return result;
    result = uart_param_config(CHASSIS_UART, &config);
    if (result == ESP_OK) {
        result = uart_set_pin(CHASSIS_UART, CHASSIS_TX, CHASSIS_RX,
                              UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (result != ESP_OK) {
        uart_driver_delete(CHASSIS_UART);
        return result;
    }
    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL) {
        uart_driver_delete(CHASSIS_UART);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(chassis_sender, "chassis_sender", 3072, NULL, 9, NULL) != pdPASS) {
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        uart_driver_delete(CHASSIS_UART);
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG,
             "UART bridge ready; adjustable 40-100%% speed, left straight trim=%.1f%% and 300 ms deadman",
             CHASSIS_STRAIGHT_LEFT_GAIN_PER_MILLE / 10.0f);
    return ESP_OK;
}

bool baby_tank_chassis_bridge_ready(void)
{
    return s_ready;
}

bool baby_tank_chassis_bridge_link_verified(void)
{
    return s_link_verified;
}

uint32_t baby_tank_chassis_bridge_control_epoch(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t epoch = s_control_epoch;
    portEXIT_CRITICAL(&s_lock);
    return epoch;
}

esp_err_t baby_tank_chassis_bridge_drive(int16_t left_per_mille,
                                         int16_t right_per_mille)
{
    if (baby_tank_policy_is_locked()) return ESP_ERR_INVALID_STATE;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (left_per_mille < -CHASSIS_MAX_PER_MILLE ||
        left_per_mille > CHASSIS_MAX_PER_MILLE ||
        right_per_mille < -CHASSIS_MAX_PER_MILLE ||
        right_per_mille > CHASSIS_MAX_PER_MILLE) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (!s_link_verified) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ++s_control_epoch;
    int16_t left = left_per_mille;
    int16_t right = right_per_mille;
    portENTER_CRITICAL(&s_lock);
    s_left = left;
    s_right = right;
    s_updated_us = esp_timer_get_time();
    s_active = true;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t result = send_motion(left, right);
    if (result != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_active = false;
        portEXIT_CRITICAL(&s_lock);
        send_motion(0, 0);
    }
    xSemaphoreGive(s_tx_lock);
    return result;
}

esp_err_t baby_tank_chassis_bridge_stop(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    ++s_control_epoch;
    portENTER_CRITICAL(&s_lock);
    s_left = 0;
    s_right = 0;
    s_updated_us = esp_timer_get_time();
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t result = send_motion(0, 0);
    if (result == ESP_OK && !s_link_verified) {
        s_link_verified = true;
        ESP_LOGI(TAG, "chassis link verified by exact UART STOP echo");
    }
    xSemaphoreGive(s_tx_lock);
    return result;
}

esp_err_t baby_tank_chassis_bridge_get_battery_percent(uint8_t *percent)
{
    if (!s_ready || percent == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_flush_input(CHASSIS_UART);
    static const char query[] = "b\n";
    int sent = uart_write_bytes(CHASSIS_UART, query, sizeof(query) - 1U);
    if (sent != (int)sizeof(query) - 1) {
        xSemaphoreGive(s_tx_lock);
        return ESP_FAIL;
    }
    char response[12] = {0};
    int received = uart_read_bytes(CHASSIS_UART, response,
                                   sizeof(response) - 1U,
                                   pdMS_TO_TICKS(CHASSIS_ECHO_TIMEOUT_MS));
    xSemaphoreGive(s_tx_lock);
    if (received <= 0) return ESP_ERR_TIMEOUT;
    unsigned value = 0U;
    char trailing = '\0';
    if (sscanf(response, "b%u%c", &value, &trailing) != 2 ||
        trailing != '\n' || value > 100U) {
        /* The stock chassis echoes "b\n" and has no telemetry response. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    *percent = (uint8_t)value;
    return ESP_OK;
}

esp_err_t baby_tank_chassis_bridge_light_mode(uint8_t mode)
{
    if (baby_tank_policy_is_locked()) return ESP_ERR_INVALID_STATE;
    if (!s_ready || (mode != 2U && mode != 6U && mode != 8U)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (!s_link_verified) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    char command[8];
    int length = snprintf(command, sizeof(command), "w%u\n", (unsigned)mode);
    esp_err_t result = length > 0 && length < (int)sizeof(command)
                           ? send_command(command, length) : ESP_FAIL;
    if (result != ESP_OK) {
        ++s_control_epoch;
        portENTER_CRITICAL(&s_lock);
        s_active = false;
        portEXIT_CRITICAL(&s_lock);
        send_motion(0, 0);
    }
    xSemaphoreGive(s_tx_lock);
    return result;
}

static esp_err_t pulse_internal(int16_t left_per_mille,
                                int16_t right_per_mille,
                                uint16_t duration_ms, bool guarded,
                                uint32_t expected_epoch)
{
    if (baby_tank_policy_is_locked()) return ESP_ERR_INVALID_STATE;
    int16_t left_magnitude = left_per_mille < 0
        ? (int16_t)-left_per_mille : left_per_mille;
    int16_t right_magnitude = right_per_mille < 0
        ? (int16_t)-right_per_mille : right_per_mille;
    if (!s_ready || duration_ms < 2000U || duration_ms > 9000U ||
        left_magnitude < 600 || left_magnitude > CHASSIS_MAX_PER_MILLE ||
        right_magnitude < 600 || right_magnitude > CHASSIS_MAX_PER_MILLE) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (guarded && s_control_epoch != expected_epoch) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    bool busy;
    portENTER_CRITICAL(&s_lock);
    busy = s_active && esp_timer_get_time() - s_updated_us < CHASSIS_DEADMAN_US;
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = send_motion(0, 0);
    if (result == ESP_OK && !s_link_verified) {
        s_link_verified = true;
        ESP_LOGI(TAG, "chassis link verified by exact UART STOP echo");
    }
    if (guarded && s_control_epoch != expected_epoch) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t epoch = s_control_epoch;
    xSemaphoreGive(s_tx_lock);
    if (result != ESP_OK) return result;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    while (esp_timer_get_time() < deadline_us) {
        if (baby_tank_policy_is_locked()) {
            (void)baby_tank_chassis_bridge_stop();
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        if (s_control_epoch != epoch || !s_link_verified ||
            baby_tank_policy_is_locked()) {
            xSemaphoreGive(s_tx_lock);
            if (baby_tank_policy_is_locked())
                (void)baby_tank_chassis_bridge_stop();
            return ESP_ERR_INVALID_STATE;
        }
        portENTER_CRITICAL(&s_lock);
        s_left = left_per_mille;
        s_right = right_per_mille;
        s_updated_us = esp_timer_get_time();
        s_active = true;
        portEXIT_CRITICAL(&s_lock);
        result = send_motion(left_per_mille, right_per_mille);
        if (result != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_active = false;
            portEXIT_CRITICAL(&s_lock);
            send_motion(0, 0);
            xSemaphoreGive(s_tx_lock);
            return result;
        }
        xSemaphoreGive(s_tx_lock);
        int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms > CHASSIS_SEND_PERIOD_MS) {
            remaining_ms = CHASSIS_SEND_PERIOD_MS;
        }
        if (remaining_ms > 0) vTaskDelay(pdMS_TO_TICKS(remaining_ms));
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (s_control_epoch != epoch || !s_link_verified) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    s_left = 0;
    s_right = 0;
    portEXIT_CRITICAL(&s_lock);
    result = send_motion(0, 0);
    xSemaphoreGive(s_tx_lock);
    return result;
}

esp_err_t baby_tank_chassis_bridge_pulse(int16_t left_per_mille,
                                         int16_t right_per_mille,
                                         uint16_t duration_ms)
{
    return pulse_internal(left_per_mille, right_per_mille, duration_ms,
                          false, 0U);
}

esp_err_t baby_tank_chassis_bridge_pulse_guarded(int16_t left_per_mille,
                                                 int16_t right_per_mille,
                                                 uint16_t duration_ms,
                                                 uint32_t expected_epoch)
{
    return pulse_internal(left_per_mille, right_per_mille, duration_ms,
                          true, expected_epoch);
}
