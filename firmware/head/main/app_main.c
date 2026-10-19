#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "face_controller.h"
#include "conversation.h"
#include "audio_io.h"
#include "cloud_client.h"
#include "network_runtime.h"
#include "power_runtime.h"
#include "touch_wake.h"
#include "voice_runtime.h"
#include "chassis_debug_console.h"

static const char *TAG = "baby_tank";

#define DREAM_REFRESH_SUCCESS_MS (20U * 60U * 1000U)
#define DREAM_REFRESH_RETRY_MS (15U * 60U * 1000U)
#define DREAM_REFRESH_TASK_STACK 8192U
#define MAIN_RUNTIME_POLL_MS 250U
#define MEMORY_LOG_INTERVAL_MS 60000U

static volatile bool s_dream_refresh_running;
static volatile TickType_t s_next_dream_refresh;

static void dream_refresh_worker(void *argument)
{
    (void)argument;
    esp_err_t refresh = ESP_ERR_INVALID_STATE;
    baby_tank_presence_state_t presence;
    if (baby_tank_face_get_presence_state(&presence) == ESP_OK &&
        presence == BABY_TANK_PRESENCE_SLEEPING &&
        baby_tank_network_internet_phase() == BABY_TANK_INTERNET_ONLINE) {
        refresh = baby_tank_cloud_refresh_sleep_thought();
    }
    uint32_t delay_ms = refresh == ESP_OK
        ? DREAM_REFRESH_SUCCESS_MS : DREAM_REFRESH_RETRY_MS;
    s_next_dream_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    if (refresh != ESP_OK) {
        ESP_LOGW(TAG, "sleep dream refresh deferred: %s",
                 esp_err_to_name(refresh));
    }
    s_dream_refresh_running = false;
    vTaskDelete(NULL);
}

static void on_touch_wake(baby_tank_touch_zone_t zone, void *context)
{
    (void)context;
    /* Every valid touch wakes NORMAL immediately, even when the subsequent
     * voice reaction is rate-limited or unavailable. */
    (void)baby_tank_face_note_activity();
    int8_t horizontal = 0;
    int8_t vertical = 0;
    const char *zone_name = "top";
    if (zone == BABY_TANK_TOUCH_LEFT) {
        horizontal = -100;
        zone_name = "left";
    } else if (zone == BABY_TANK_TOUCH_RIGHT) {
        horizontal = 100;
        zone_name = "right";
    } else {
        vertical = -100;
    }
    esp_err_t gaze_result = baby_tank_display_look_at(
        horizontal, vertical, zone == BABY_TANK_TOUCH_TOP ? 1100U : 1400U);
    if (gaze_result != ESP_OK) {
        ESP_LOGW(TAG, "%s touch gaze rejected: %s", zone_name,
                 esp_err_to_name(gaze_result));
    }
    esp_err_t result;
    if (zone == BABY_TANK_TOUCH_LEFT || zone == BABY_TANK_TOUCH_RIGHT) {
        result = baby_tank_voice_runtime_request_touch_reaction(
            zone == BABY_TANK_TOUCH_LEFT ? BABY_TANK_TOUCH_REACTION_LEFT :
                                           BABY_TANK_TOUCH_REACTION_RIGHT);
    } else {
        result = baby_tank_voice_runtime_toggle_listen();
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "%s touch voice action rejected: %s", zone_name,
                 esp_err_to_name(result));
    }
}

static void log_hardware_summary(void)
{
    esp_chip_info_t chip_info = {0};
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);

    ESP_LOGI(TAG,
             "chip=ESP32-S3 cores=%u revision=%u.%u idf=%s",
             chip_info.cores,
             chip_info.revision / 100,
             chip_info.revision % 100,
             esp_get_idf_version());

    esp_err_t flash_result = esp_flash_get_size(NULL, &flash_size);
    if (flash_result == ESP_OK) {
        ESP_LOGI(TAG, "flash=%" PRIu32 " bytes (%" PRIu32 " MiB)",
                 flash_size,
                 flash_size / (1024U * 1024U));
    } else {
        ESP_LOGE(TAG, "flash size probe failed: %s", esp_err_to_name(flash_result));
    }

    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) {
        ESP_LOGI(TAG, "psram=%zu bytes (%zu MiB)",
                 psram_size,
                 psram_size / (1024U * 1024U));
    } else {
        ESP_LOGW(TAG, "PSRAM is unavailable");
    }

    ESP_LOGI(TAG,
             "heap_internal_total=%zu heap_internal_free=%zu "
             "heap_spiram_total=%zu heap_spiram_free=%zu reset_reason=%d",
             heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (int)esp_reset_reason());
}

static void log_heap_checkpoint(const char *phase)
{
    ESP_LOGI(TAG,
             "heap phase=%s internal_free=%zu internal_largest=%zu spiram_free=%zu",
             phase,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "Baby Tank head bring-up firmware");
    ESP_LOGI(TAG, "project=%s version=%s built=%s %s",
             app->project_name,
             app->version,
             app->date,
             app->time);
    log_hardware_summary();

    esp_err_t face_result = baby_tank_face_controller_start();
    if (face_result == ESP_OK) {
        ESP_LOGI(TAG, "AI-ready animated face controller passed");
    } else {
        ESP_LOGE(TAG, "face controller failed: %s", esp_err_to_name(face_result));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    esp_err_t conversation_result = baby_tank_conversation_init();
    if (conversation_result == ESP_OK) {
        ESP_LOGI(TAG, "AI conversation-to-expression adapter passed");
    } else {
        ESP_LOGE(TAG, "conversation adapter failed: %s",
                 esp_err_to_name(conversation_result));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    /* Wi-Fi and BLE require contiguous internal driver/DMA memory. Reserve it
     * before the AFE and WakeNet allocate their working buffers. */
    log_heap_checkpoint("before_network");
    esp_err_t network_result = baby_tank_network_start();
    if (network_result == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(TAG, "waiting for encrypted BLE Wi-Fi provisioning");
        baby_tank_conversation_set_phase(BABY_TANK_CONVERSATION_DISCONNECTED);
    } else if (network_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "cloud network unavailable at boot: %s; local wake remains active",
                 esp_err_to_name(network_result));
    }
    /* ESP-IDF enables modem sleep by default. Product policy allows it only
     * after the face state machine has entered real sleep. */
    esp_err_t wifi_power_result =
        baby_tank_network_set_sleep_power_save(false);
    if (wifi_power_result != ESP_OK) {
        ESP_LOGW(TAG, "unable to disable awake Wi-Fi power save: %s",
                 esp_err_to_name(wifi_power_result));
    }
    log_heap_checkpoint("after_network");
    esp_err_t dream_restore = baby_tank_cloud_restore_sleep_thought();
    if (dream_restore != ESP_OK && dream_restore != ESP_ERR_NVS_NOT_FOUND &&
        dream_restore != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "cached dream caption unavailable: %s",
                 esp_err_to_name(dream_restore));
    }
    esp_err_t power_result = baby_tank_power_runtime_start();
    if (power_result != ESP_OK) {
        ESP_LOGW(TAG, "battery monitoring unavailable: %s",
                 esp_err_to_name(power_result));
    }

    esp_err_t chassis_console_result = baby_tank_chassis_debug_console_start();
    if (chassis_console_result != ESP_OK) {
        ESP_LOGW(TAG, "chassis USB debug console unavailable: %s",
                 esp_err_to_name(chassis_console_result));
    }

    esp_err_t audio_result = baby_tank_audio_io_init();
    if (audio_result == ESP_OK) {
        ESP_LOGI(TAG, "ES8311 audio I/O ready");
    } else {
        ESP_LOGE(TAG, "audio I/O failed: %s", esp_err_to_name(audio_result));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    log_heap_checkpoint("after_audio");

    esp_err_t voice_result = baby_tank_voice_runtime_start();
    if (voice_result == ESP_OK) {
        ESP_LOGI(TAG, "local wake and cloud dialogue runtime ready");
    } else {
        ESP_LOGE(TAG, "voice runtime failed: %s",
                 esp_err_to_name(voice_result));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    log_heap_checkpoint("after_voice");

    esp_err_t touch_result = baby_tank_touch_wake_start(on_touch_wake, NULL);
    if (touch_result == ESP_OK) {
        ESP_LOGI(TAG, "top touch conversation wake ready");
    } else {
        ESP_LOGW(TAG, "touch wake unavailable: %s",
                 esp_err_to_name(touch_result));
    }

    ESP_LOGI(TAG,
             "camera is deferred until remote mode or an AI vision capture");
    log_heap_checkpoint("after_camera");
    baby_tank_voice_runtime_log_memory();

    ESP_LOGI(TAG, "robot-baby idle runtime ready");
    TickType_t next_memory_log = xTaskGetTickCount() +
        pdMS_TO_TICKS(MEMORY_LOG_INTERVAL_MS);
    s_next_dream_refresh = 0U;
    bool wifi_sleeping = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_RUNTIME_POLL_MS));
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_memory_log) >= 0) {
            baby_tank_voice_runtime_log_memory();
            next_memory_log = now + pdMS_TO_TICKS(MEMORY_LOG_INTERVAL_MS);
        }
        baby_tank_presence_state_t presence;
        if (baby_tank_face_get_presence_state(&presence) == ESP_OK) {
            bool should_sleep_wifi =
                presence == BABY_TANK_PRESENCE_SLEEPING;
            if (should_sleep_wifi != wifi_sleeping &&
                baby_tank_network_set_sleep_power_save(
                    should_sleep_wifi) == ESP_OK) {
                wifi_sleeping = should_sleep_wifi;
            }
        }
        if (baby_tank_face_get_presence_state(&presence) == ESP_OK &&
            presence == BABY_TANK_PRESENCE_SLEEPING &&
            baby_tank_network_internet_phase() == BABY_TANK_INTERNET_ONLINE &&
            !s_dream_refresh_running &&
            (s_next_dream_refresh == 0U ||
             (int32_t)(now - s_next_dream_refresh) >= 0)) {
            s_dream_refresh_running = true;
            BaseType_t created = xTaskCreate(
                dream_refresh_worker, "dream_refresh",
                DREAM_REFRESH_TASK_STACK, NULL, 4, NULL);
            if (created != pdPASS) {
                s_dream_refresh_running = false;
                s_next_dream_refresh = now +
                    pdMS_TO_TICKS(DREAM_REFRESH_RETRY_MS);
                ESP_LOGW(TAG, "unable to start dream refresh worker");
            }
        }
    }
}
