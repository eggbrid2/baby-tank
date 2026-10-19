#include "chassis_debug_console.h"

#include <stdbool.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "chassis_bridge.h"

#define CONSOLE_TASK_STACK 2560
#define CONSOLE_TASK_PRIORITY 5
#define TEST_SPEED_PER_MILLE 600
#define TEST_PULSE_MS 700
#define TEST_REFRESH_MS 80

static const char *TAG = "baby_tank_motor_test";
static bool s_started;

static void run_pulse(int16_t left, int16_t right, const char *name)
{
    /* Stop before and after every pulse. The UART bridge independently clamps
     * speed to 60% and applies a 300 ms deadman if this task is interrupted. */
    esp_err_t result = baby_tank_chassis_bridge_stop();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "motor test blocked: chassis echo check failed: %s",
                 esp_err_to_name(result));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "motor test start: %s left=%d right=%d duration=%dms",
             name, left, right, TEST_PULSE_MS);
    int elapsed_ms = 0;
    while (elapsed_ms < TEST_PULSE_MS) {
        result = baby_tank_chassis_bridge_drive(left, right);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "motor test refresh rejected at %dms: %s",
                     elapsed_ms, esp_err_to_name(result));
            break;
        }
        int remaining_ms = TEST_PULSE_MS - elapsed_ms;
        int delay_ms = remaining_ms < TEST_REFRESH_MS
                           ? remaining_ms
                           : TEST_REFRESH_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        elapsed_ms += delay_ms;
    }
    result = baby_tank_chassis_bridge_stop();
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "motor test stopped and acknowledged: %s", name);
    } else {
        ESP_LOGE(TAG, "motor test stop was not acknowledged: %s: %s",
                 name, esp_err_to_name(result));
    }
}

static void execute_line(const char *line)
{
    if (strcmp(line, "motor stop") == 0) {
        esp_err_t result = baby_tank_chassis_bridge_stop();
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "motor stop acknowledged by chassis");
        } else {
            ESP_LOGW(TAG, "motor stop sent without chassis acknowledgment: %s",
                     esp_err_to_name(result));
        }
    } else if (strcmp(line, "motor forward") == 0) {
        run_pulse(TEST_SPEED_PER_MILLE, TEST_SPEED_PER_MILLE, "forward");
    } else if (strcmp(line, "motor backward") == 0) {
        run_pulse(-TEST_SPEED_PER_MILLE, -TEST_SPEED_PER_MILLE, "backward");
    } else if (strcmp(line, "motor left") == 0) {
        run_pulse(TEST_SPEED_PER_MILLE, 0, "left-track");
    } else if (strcmp(line, "motor right") == 0) {
        run_pulse(0, TEST_SPEED_PER_MILLE, "right-track");
    } else if (strcmp(line, "motor turn-left") == 0) {
        run_pulse(-TEST_SPEED_PER_MILLE, TEST_SPEED_PER_MILLE, "turn-left");
    } else if (strcmp(line, "motor turn-right") == 0) {
        run_pulse(TEST_SPEED_PER_MILLE, -TEST_SPEED_PER_MILLE, "turn-right");
    } else if (line[0] != '\0') {
        ESP_LOGW(TAG, "unknown USB motor test command");
    }
}

static void console_task(void *argument)
{
    (void)argument;
    char line[32] = {0};
    size_t length = 0;
    ESP_LOGI(TAG,
             "USB motor test ready: motor stop|left|right|forward|backward|turn-left|turn-right");
    for (;;) {
        char byte = 0;
        int count = usb_serial_jtag_read_bytes(
            &byte, 1, pdMS_TO_TICKS(20));
        if (count == 1) {
            if (byte == '\r' || byte == '\n') {
                if (length > 0) {
                    line[length] = '\0';
                    execute_line(line);
                    length = 0;
                }
            } else if (length + 1 < sizeof(line)) {
                line[length++] = byte;
            } else {
                length = 0;
                ESP_LOGW(TAG, "USB motor test command too long");
            }
        }
    }
}

esp_err_t baby_tank_chassis_debug_console_start(void)
{
    if (s_started) return ESP_OK;
    if (!baby_tank_chassis_bridge_ready()) return ESP_ERR_INVALID_STATE;
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    esp_err_t result = usb_serial_jtag_driver_install(&config);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(console_task, "motor_usb_test", CONSOLE_TASK_STACK, NULL,
                    CONSOLE_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
