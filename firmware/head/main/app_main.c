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
#include "runtime_policy.h"
#include "state_event_queue.h"
#include "interaction_memory.h"
#include "chassis_debug_console.h"
#include "chassis_bridge.h"

static const char *TAG = "baby_tank";

#define DREAM_REFRESH_SUCCESS_MS (20U * 60U * 1000U)
#define DREAM_REFRESH_RETRY_MS (15U * 60U * 1000U)
#define DREAM_REFRESH_TASK_STACK 8192U
#define MOOD_REFRESH_SUCCESS_MS (5U * 60U * 1000U)
#define MOOD_REFRESH_RETRY_MS (60U * 1000U)
#define MOOD_REFRESH_TASK_STACK 8192U
#define MAIN_RUNTIME_POLL_MS 250U
#define MEMORY_LOG_INTERVAL_MS 60000U
#define TEXT_POLL_TASK_STACK 7168U
#define RUNTIME_SUPERVISOR_INTERVAL_MS 5000U
#define RUNTIME_PHASE_STALL_MS (180U * 1000U)
#define RUNTIME_LOW_HEAP_FREE_BYTES (8U * 1024U)
#define RUNTIME_LOW_HEAP_LARGEST_BYTES (4U * 1024U)
#define RUNTIME_LOW_HEAP_SAMPLES 6U
#define RUNTIME_MAX_RECOVERY_FAILURES 2U

static volatile bool s_dream_refresh_running;
static volatile TickType_t s_next_dream_refresh;
static volatile bool s_mood_refresh_running;
static volatile TickType_t s_next_mood_refresh;
static volatile bool s_event_upload_running;
static volatile TickType_t s_next_event_upload;
static volatile bool s_text_poll_running;
static volatile TickType_t s_next_text_poll;

static bool supervised_conversation_phase(
    baby_tank_conversation_phase_t phase)
{
    return phase == BABY_TANK_CONVERSATION_LISTENING ||
           phase == BABY_TANK_CONVERSATION_USER_SPEAKING ||
           phase == BABY_TANK_CONVERSATION_RECOGNIZING ||
           phase == BABY_TANK_CONVERSATION_THINKING ||
           phase == BABY_TANK_CONVERSATION_AI_SPEAKING;
}

static esp_err_t recover_foreground_runtime(const char *reason)
{
    ESP_LOGW(TAG, "runtime supervisor recovery reason=%s", reason);
    (void)baby_tank_chassis_bridge_stop();
    baby_tank_cloud_voice_session_close();
    if (baby_tank_voice_runtime_remote_mode()) {
        return baby_tank_voice_runtime_set_remote_mode(false);
    }
    if (baby_tank_voice_runtime_is_ready()) {
        return baby_tank_voice_runtime_cancel_conversation();
    }
    return baby_tank_voice_runtime_start();
}

static void reboot_after_recovery_failure(const char *reason)
{
    ESP_LOGE(TAG, "runtime supervisor reboot reason=%s", reason);
    (void)baby_tank_chassis_bridge_stop();
    baby_tank_cloud_voice_session_close();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static bool text_wait_available(void)
{
    return !baby_tank_policy_is_locked() &&
        baby_tank_network_internet_phase() == BABY_TANK_INTERNET_ONLINE &&
        baby_tank_network_cloud_is_ready() &&
        !s_dream_refresh_running && !s_mood_refresh_running &&
        !s_event_upload_running &&
        !baby_tank_cloud_voice_session_active() &&
        baby_tank_voice_runtime_can_accept_text();
}

static void text_poll_worker(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG, "fallback App-text poll started");
    char job_id[40] = {0};
    esp_err_t result = baby_tank_cloud_wait_text_turn(
        job_id, sizeof(job_id), text_wait_available);
    if (result == ESP_OK) {
        baby_tank_cloud_clear_text_notice();
        esp_err_t queued = baby_tank_voice_runtime_request_cloud_text(job_id);
        if (queued != ESP_OK) {
            (void)baby_tank_cloud_complete_text_turn(job_id, "blocked");
            ESP_LOGW(TAG, "cloud text turn deferred: %s",
                     esp_err_to_name(queued));
        }
    } else if (result != ESP_ERR_INVALID_STATE &&
               result != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "cloud text wait unavailable: %s",
                 esp_err_to_name(result));
    }
    s_next_text_poll = xTaskGetTickCount() + pdMS_TO_TICKS(
        result == ESP_ERR_INVALID_STATE ? 500U : 30000U);
    ESP_LOGI(TAG, "fallback App-text poll finished: %s",
             esp_err_to_name(result));
    s_text_poll_running = false;
    vTaskDelete(NULL);
}

static void event_upload_worker(void *argument)
{
    (void)argument;
    bool retry = false;
    if (baby_tank_cloud_wait_text_idle(500U) != ESP_OK) retry = true;
    for (unsigned count = 0; !retry && count < 4U; ++count) {
        char event_id[37];
        baby_tank_state_event_kind_t kind;
        if (!baby_tank_state_event_peek(event_id, &kind)) break;
        if (baby_tank_network_internet_phase() != BABY_TANK_INTERNET_ONLINE) {
            retry = true;
            break;
        }
        const char *name = kind == BABY_TANK_STATE_EVENT_SIDE_TOUCH
            ? "side_touch" : "scene_completed";
        bool permanent_rejection = false;
        esp_err_t result = baby_tank_cloud_upload_character_event(
            event_id, name, &permanent_rejection);
        if (result == ESP_OK || permanent_rejection) {
            baby_tank_state_event_mark_uploaded(event_id);
        } else {
            retry = true;
            break;
        }
    }
    s_next_event_upload = xTaskGetTickCount() + pdMS_TO_TICKS(
        retry ? 30000U : 5000U);
    s_next_text_poll = xTaskGetTickCount() + pdMS_TO_TICKS(30000U);
    s_event_upload_running = false;
    vTaskDelete(NULL);
}

static void mood_refresh_worker(void *argument)
{
    (void)argument;
    esp_err_t result = baby_tank_cloud_wait_text_idle(500U);
    esp_err_t identity = result == ESP_OK
        ? baby_tank_cloud_fetch_verified_identity() : result;
    if (result == ESP_OK) result = baby_tank_cloud_refresh_character_mood();
    if (result == ESP_OK && identity != ESP_OK) result = identity;
    s_next_mood_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(
        result == ESP_OK ? MOOD_REFRESH_SUCCESS_MS : MOOD_REFRESH_RETRY_MS);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "idle mood refresh deferred: %s",
                 esp_err_to_name(result));
    }
    s_next_text_poll = xTaskGetTickCount() + pdMS_TO_TICKS(30000U);
    s_mood_refresh_running = false;
    vTaskDelete(NULL);
}

static void dream_refresh_worker(void *argument)
{
    (void)argument;
    esp_err_t refresh = ESP_ERR_INVALID_STATE;
    baby_tank_presence_state_t presence;
    if (baby_tank_face_get_presence_state(&presence) == ESP_OK &&
        presence == BABY_TANK_PRESENCE_SLEEPING &&
        baby_tank_network_internet_phase() == BABY_TANK_INTERNET_ONLINE &&
        baby_tank_cloud_wait_text_idle(500U) == ESP_OK) {
        refresh = baby_tank_cloud_refresh_sleep_thought();
    }
    uint32_t delay_ms = refresh == ESP_OK
        ? DREAM_REFRESH_SUCCESS_MS : DREAM_REFRESH_RETRY_MS;
    s_next_dream_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    if (refresh != ESP_OK) {
        ESP_LOGW(TAG, "sleep dream refresh deferred: %s",
                 esp_err_to_name(refresh));
    }
    s_next_text_poll = xTaskGetTickCount() + pdMS_TO_TICKS(30000U);
    s_dream_refresh_running = false;
    vTaskDelete(NULL);
}

static void on_touch_wake(baby_tank_touch_zone_t zone, void *context)
{
    (void)context;
    if (baby_tank_policy_is_locked()) {
        if (zone == BABY_TANK_TOUCH_TOP) {
            (void)baby_tank_chassis_bridge_stop();
        }
        return;
    }
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
        if (result == ESP_OK) {
            (void)baby_tank_state_event_enqueue(
                BABY_TANK_STATE_EVENT_SIDE_TOUCH);
        }
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
    baby_tank_interaction_memory_init();

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
    TickType_t next_supervisor_check = xTaskGetTickCount() +
        pdMS_TO_TICKS(RUNTIME_SUPERVISOR_INTERVAL_MS);
    TickType_t phase_progress_at = xTaskGetTickCount();
    baby_tank_conversation_phase_t supervised_phase =
        BABY_TANK_CONVERSATION_IDLE;
    uint32_t supervised_turn = 0U;
    uint8_t low_heap_samples = 0U;
    uint8_t low_heap_recoveries = 0U;
    uint8_t recovery_failures = 0U;
    s_next_dream_refresh = 0U;
    /* Refresh the trusted clock and night policy immediately after an online
     * cold boot. Waiting for the normal five-minute mood cadence leaves a
     * newly powered device awake past bedtime with clock quality unknown. */
    s_next_mood_refresh = xTaskGetTickCount();
    s_next_text_poll = xTaskGetTickCount() + pdMS_TO_TICKS(30000U);
    /* Treat an already-restored night lock as an entry transition. Otherwise
     * a reboot during 23:00-06:00 renders a sleeping face but never cancels
     * the voice/remote foreground runtime. */
    bool was_night_locked = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_RUNTIME_POLL_MS));
        TickType_t now = xTaskGetTickCount();
        (void)baby_tank_policy_tick();
        bool night_locked = baby_tank_policy_is_locked();
        if (night_locked && !was_night_locked) {
            (void)baby_tank_chassis_bridge_stop();
            baby_tank_voice_runtime_enter_night_lock();
            if (baby_tank_voice_runtime_remote_mode()) {
                (void)baby_tank_voice_runtime_set_remote_mode(false);
            }
        } else if (!night_locked && was_night_locked) {
            baby_tank_voice_runtime_exit_night_lock();
            (void)baby_tank_face_note_activity();
            ESP_LOGI(TAG, "morning policy unlock restored awake face");
        }
        was_night_locked = night_locked;
        if ((int32_t)(now - next_memory_log) >= 0) {
            baby_tank_voice_runtime_log_memory();
            next_memory_log = now + pdMS_TO_TICKS(MEMORY_LOG_INTERVAL_MS);
        }
        if ((int32_t)(now - next_supervisor_check) >= 0) {
            next_supervisor_check = now +
                pdMS_TO_TICKS(RUNTIME_SUPERVISOR_INTERVAL_MS);
            bool recovered_this_check = false;
            baby_tank_conversation_snapshot_t health = {0};
            if (baby_tank_conversation_get_snapshot(&health) == ESP_OK) {
                if (health.phase != supervised_phase ||
                    health.turn_id != supervised_turn) {
                    supervised_phase = health.phase;
                    supervised_turn = health.turn_id;
                    phase_progress_at = now;
                    recovery_failures = 0U;
                } else if (supervised_conversation_phase(health.phase) &&
                    (uint32_t)((now - phase_progress_at) * portTICK_PERIOD_MS) >=
                        RUNTIME_PHASE_STALL_MS) {
                    ESP_LOGE(TAG,
                             "conversation stalled phase=%d turn=%lu",
                             (int)health.phase,
                             (unsigned long)health.turn_id);
                    esp_err_t recovered =
                        recover_foreground_runtime("conversation_stalled");
                    recovered_this_check = true;
                    phase_progress_at = now;
                    if (recovered == ESP_OK) recovery_failures = 0U;
                    else if (++recovery_failures >=
                             RUNTIME_MAX_RECOVERY_FAILURES) {
                        reboot_after_recovery_failure(
                            "conversation_recovery_failed");
                    }
                }
            }

            size_t internal_free =
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t internal_largest =
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            bool critically_low =
                internal_free < RUNTIME_LOW_HEAP_FREE_BYTES ||
                internal_largest < RUNTIME_LOW_HEAP_LARGEST_BYTES;
            if (!critically_low) {
                low_heap_samples = 0U;
                low_heap_recoveries = 0U;
            } else if (low_heap_samples < UINT8_MAX) {
                ++low_heap_samples;
            }
            if (!recovered_this_check &&
                low_heap_samples >= RUNTIME_LOW_HEAP_SAMPLES) {
                ESP_LOGE(TAG,
                         "critical heap persisted free=%zu largest=%zu recovery=%u",
                         internal_free, internal_largest,
                         (unsigned)low_heap_recoveries + 1U);
                low_heap_samples = 0U;
                if (++low_heap_recoveries >= 2U) {
                    reboot_after_recovery_failure("critical_heap_persisted");
                }
                esp_err_t recovered =
                    recover_foreground_runtime("critical_heap");
                if (recovered == ESP_OK) recovery_failures = 0U;
                else if (++recovery_failures >=
                         RUNTIME_MAX_RECOVERY_FAILURES) {
                    reboot_after_recovery_failure("heap_recovery_failed");
                }
            }
        }
        (void)baby_tank_state_events_persist_if_dirty();
        if (baby_tank_cloud_text_notice_pending() &&
            baby_tank_voice_runtime_can_accept_text()) {
            char job_id[40] = {0};
            if (baby_tank_cloud_take_text_notice(job_id, sizeof(job_id))) {
                esp_err_t queued =
                    baby_tank_voice_runtime_request_cloud_text(job_id);
                if (queued != ESP_OK) {
                    (void)baby_tank_cloud_complete_text_turn(job_id,
                                                              "blocked");
                    ESP_LOGW(TAG, "warm App-text turn deferred: %s",
                             esp_err_to_name(queued));
                }
            }
        }
        if (!s_text_poll_running && text_wait_available() &&
            (baby_tank_cloud_text_notice_pending() ||
             (int32_t)(now - s_next_text_poll) >= 0)) {
            s_text_poll_running = true;
            if (xTaskCreate(text_poll_worker, "text_poll",
                            TEXT_POLL_TASK_STACK, NULL, 4, NULL) != pdPASS) {
                s_text_poll_running = false;
                s_next_text_poll = now + pdMS_TO_TICKS(5000U);
            }
        }
        char pending_event_id[37];
        baby_tank_state_event_kind_t pending_event_kind;
        baby_tank_conversation_snapshot_t event_dialogue = {0};
        if (!s_event_upload_running && !s_mood_refresh_running &&
            !s_dream_refresh_running &&
            baby_tank_network_internet_phase() == BABY_TANK_INTERNET_ONLINE &&
            baby_tank_conversation_get_snapshot(&event_dialogue) == ESP_OK &&
            event_dialogue.phase == BABY_TANK_CONVERSATION_IDLE &&
            baby_tank_state_event_peek(pending_event_id, &pending_event_kind) &&
            (int32_t)(now - s_next_event_upload) >= 0) {
            /* HTTPS background work and the voice socket both need the single
             * hardware crypto engine. Never run their TLS handshakes together. */
            baby_tank_cloud_voice_session_close();
            s_event_upload_running = true;
            if (xTaskCreate(event_upload_worker, "state_events", 6144U,
                            NULL, 4, NULL) != pdPASS) {
                s_event_upload_running = false;
                s_next_event_upload = now + pdMS_TO_TICKS(30000U);
            }
        }
        baby_tank_presence_state_t presence;
        /* A sleeping face is a presentation state, not a Wi-Fi power policy.
         * Modem sleep remains off until an independent battery policy exists. */
        if (baby_tank_face_get_presence_state(&presence) == ESP_OK &&
            presence == BABY_TANK_PRESENCE_SLEEPING &&
            !baby_tank_policy_is_locked() &&
            baby_tank_network_internet_phase() == BABY_TANK_INTERNET_ONLINE &&
            !s_dream_refresh_running &&
            !s_mood_refresh_running &&
            (s_next_dream_refresh == 0U ||
             (int32_t)(now - s_next_dream_refresh) >= 0)) {
            baby_tank_cloud_voice_session_close();
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
        baby_tank_conversation_snapshot_t dialogue = {0};
        if (baby_tank_face_get_presence_state(&presence) == ESP_OK &&
            baby_tank_conversation_get_snapshot(&dialogue) == ESP_OK &&
            dialogue.phase == BABY_TANK_CONVERSATION_IDLE &&
            !baby_tank_voice_runtime_remote_mode() &&
            baby_tank_network_internet_phase() == BABY_TANK_INTERNET_ONLINE &&
            !s_mood_refresh_running &&
            !s_dream_refresh_running &&
            !s_event_upload_running &&
            (int32_t)(now - s_next_mood_refresh) >= 0) {
            baby_tank_cloud_voice_session_close();
            s_mood_refresh_running = true;
            BaseType_t created = xTaskCreate(
                mood_refresh_worker, "mood_refresh", MOOD_REFRESH_TASK_STACK,
                NULL, 4, NULL);
            if (created != pdPASS) {
                s_mood_refresh_running = false;
                s_next_mood_refresh = now + pdMS_TO_TICKS(MOOD_REFRESH_RETRY_MS);
                ESP_LOGW(TAG, "unable to start idle mood refresh worker");
            }
        }
        /* Start the persistent voice transport only after all due background
         * HTTPS jobs have been scheduled. This prevents two TLS handshakes
         * from starving the microphone AFE or the idle watchdog. */
        if (!night_locked && !s_text_poll_running &&
            !baby_tank_cloud_text_notice_pending() &&
            !s_dream_refresh_running && !s_mood_refresh_running &&
            !s_event_upload_running &&
            (baby_tank_network_internet_phase() == BABY_TANK_INTERNET_ONLINE ||
             (baby_tank_network_internet_phase() == BABY_TANK_INTERNET_TESTING &&
              !baby_tank_network_internet_probe_busy())) &&
            baby_tank_network_cloud_is_ready() &&
            baby_tank_voice_runtime_can_accept_text() &&
            !baby_tank_voice_runtime_remote_mode() &&
            !baby_tank_cloud_voice_session_active()) {
            (void)baby_tank_cloud_voice_session_prepare();
        }
    }
}
