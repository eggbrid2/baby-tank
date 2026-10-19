#include "diagnostics.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_io.h"
#include "camera_stream.h"
#include "chassis_bridge.h"
#include "conversation.h"
#include "display.h"
#include "face_controller.h"
#include "network_runtime.h"
#include "touch_wake.h"
#include "voice_runtime.h"

typedef enum {
    DIAGNOSTIC_STATUS = 0,
    DIAGNOSTIC_EXPRESSION = 1,
    DIAGNOSTIC_LOOK = 2,
    DIAGNOSTIC_LIGHT = 3,
    DIAGNOSTIC_SPEAKER = 4,
    DIAGNOSTIC_LISTEN = 5,
    DIAGNOSTIC_REMOTE_MODE = 6,
    DIAGNOSTIC_CANNON = 7,
    DIAGNOSTIC_VOLUME = 8,
    DIAGNOSTIC_GAME_RESULT = 9,
} diagnostic_opcode_t;

typedef enum {
    DIAGNOSTIC_OK = 0,
    DIAGNOSTIC_INVALID = 1,
    DIAGNOSTIC_UNAVAILABLE = 2,
    DIAGNOSTIC_BUSY = 3,
} diagnostic_status_t;

static portMUX_TYPE s_tone_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_tone_busy;

static void write_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static void diagnostic_tone(void *argument)
{
    unsigned variant = (unsigned)(uintptr_t)argument;
    int16_t samples[160];
    if (variant == 6U) {
        /* Let a cancelled AI/wake playback unwind before the remote-mode
         * announcement takes ownership of the shared codec. */
        vTaskDelay(pdMS_TO_TICKS(400U));
    }
    if (variant == 0U || variant >= 3U) {
        uint8_t clip = variant == 0U ? 5U :
                       variant == 5U ? (uint8_t)(3U + (esp_random() & 1U)) :
                       variant == 6U ? 2U :
                       variant >= 7U ? (uint8_t)(6U + variant - 7U) :
                       (uint8_t)(variant - 3U);
        (void)baby_tank_voice_runtime_play_phrase(clip);
        taskENTER_CRITICAL(&s_tone_lock);
        s_tone_busy = false;
        taskEXIT_CRITICAL(&s_tone_lock);
        vTaskDelete(NULL);
        return;
    }
    esp_err_t result = baby_tank_audio_io_begin_playback();
    if (result == ESP_OK) {
        uint32_t noise = 0x6d2b79f5U;
        unsigned blocks = variant == 2U ? 55U : 38U;
        for (unsigned block = 0; block < blocks && result == ESP_OK; ++block) {
            for (unsigned index = 0; index < 160U; ++index) {
                unsigned absolute = block * 160U + index;
                int value = 0;
                if (variant == 0U) {
                    bool sounding = block < 11U || (block >= 18U && block < 29U);
                    if (sounding) {
                        unsigned phase = absolute % 20U;
                        value = phase < 10U ? (int)phase : 20 - (int)phase;
                        value = value * 1000 - 5000;
                    }
                } else if (variant == 1U) {
                    unsigned period = block < 13U ? 34U :
                                      (block < 25U ? 26U : 20U);
                    unsigned phase = absolute % period;
                    value = phase < period / 2U ? (int)phase :
                            (int)period - (int)phase;
                    value = value * 7600 / (int)(period / 2U) - 3800;
                } else {
                    noise ^= noise << 13U;
                    noise ^= noise >> 17U;
                    noise ^= noise << 5U;
                    int decay = (int)(blocks - block);
                    int crack = ((int)(noise & 0xffffU) - 32768) * decay / 115;
                    int rumble = ((absolute / 46U) & 1U) != 0U ?
                                 2600 * decay / (int)blocks :
                                -2600 * decay / (int)blocks;
                    value = crack + rumble;
                    if (value > 12000) value = 12000;
                    if (value < -12000) value = -12000;
                }
                samples[index] = (int16_t)value;
            }
            baby_tank_face_set_speech_level(variant == 2U ? 72U : 54U);
            result = baby_tank_audio_io_write(samples, 160U);
        }
        baby_tank_face_set_speech_level(0U);
        baby_tank_audio_io_end_playback();
    }
    taskENTER_CRITICAL(&s_tone_lock);
    s_tone_busy = false;
    taskEXIT_CRITICAL(&s_tone_lock);
    vTaskDelete(NULL);
}

static diagnostic_status_t start_tone(uint8_t variant)
{
    if (variant > 9U) return DIAGNOSTIC_INVALID;
    taskENTER_CRITICAL(&s_tone_lock);
    if (s_tone_busy || baby_tank_audio_io_is_playing()) {
        taskEXIT_CRITICAL(&s_tone_lock);
        return DIAGNOSTIC_BUSY;
    }
    s_tone_busy = true;
    taskEXIT_CRITICAL(&s_tone_lock);
    if (xTaskCreate(diagnostic_tone, "diagnostic_tone", 3072,
                    (void *)(uintptr_t)variant, 5, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_tone_lock);
        s_tone_busy = false;
        taskEXIT_CRITICAL(&s_tone_lock);
        return DIAGNOSTIC_UNAVAILABLE;
    }
    return DIAGNOSTIC_OK;
}

static diagnostic_status_t run_command(const uint8_t *input)
{
    diagnostic_opcode_t opcode = (diagnostic_opcode_t)input[1];
    if (opcode != DIAGNOSTIC_STATUS && opcode != DIAGNOSTIC_VOLUME) {
        (void)baby_tank_face_note_activity();
    }
    switch (opcode) {
    case DIAGNOSTIC_STATUS:
        return DIAGNOSTIC_OK;
    case DIAGNOSTIC_EXPRESSION: {
        if (input[2] >= BABY_TANK_EXPRESSION_COUNT) return DIAGNOSTIC_INVALID;
        uint32_t duration = (uint32_t)input[4] |
                            ((uint32_t)input[5] << 8U);
        baby_tank_face_request_t request = {
            .expression = (baby_tank_expression_t)input[2],
            .duration_ms = duration,
            .intensity = input[3],
            .priority = BABY_TANK_FACE_PRIORITY_INTERACTION,
        };
        return baby_tank_face_submit(&request) == ESP_OK
                   ? DIAGNOSTIC_OK : DIAGNOSTIC_UNAVAILABLE;
    }
    case DIAGNOSTIC_LOOK: {
        int8_t horizontal = (int8_t)input[2];
        int8_t vertical = (int8_t)input[3];
        uint32_t duration = (uint32_t)input[4] |
                            ((uint32_t)input[5] << 8U);
        if (horizontal < -100 || horizontal > 100 ||
            vertical < -100 || vertical > 100) return DIAGNOSTIC_INVALID;
        return baby_tank_display_look_at(horizontal, vertical, duration) == ESP_OK
                   ? DIAGNOSTIC_OK : DIAGNOSTIC_UNAVAILABLE;
    }
    case DIAGNOSTIC_LIGHT:
        return baby_tank_chassis_bridge_light_mode(input[2]) == ESP_OK
                   ? DIAGNOSTIC_OK : DIAGNOSTIC_UNAVAILABLE;
    case DIAGNOSTIC_SPEAKER:
        return start_tone(input[2]);
    case DIAGNOSTIC_LISTEN:
        return baby_tank_voice_runtime_request_listen() == ESP_OK
                   ? DIAGNOSTIC_OK : DIAGNOSTIC_BUSY;
    case DIAGNOSTIC_REMOTE_MODE: {
        if (input[2] > 1U) return DIAGNOSTIC_INVALID;
        bool enabled = input[2] != 0U;
        bool announce = enabled && !baby_tank_voice_runtime_remote_mode();
        if (baby_tank_voice_runtime_set_remote_mode(enabled) != ESP_OK) {
            return DIAGNOSTIC_UNAVAILABLE;
        }
        if (announce && start_tone(6U) != DIAGNOSTIC_OK) {
            ESP_LOGW("baby_tank_diagnostics",
                     "remote mode announcement is busy");
        }
        return DIAGNOSTIC_OK;
    }
    case DIAGNOSTIC_CANNON: {
        diagnostic_status_t sound = start_tone(2U);
        if (sound != DIAGNOSTIC_OK) return sound;
        (void)baby_tank_chassis_bridge_stop();
        const baby_tank_face_request_t request = {
            .expression = BABY_TANK_EXPRESSION_CANNON,
            .duration_ms = 1700U,
            .intensity = 100U,
            .priority = BABY_TANK_FACE_PRIORITY_SAFETY,
        };
        return baby_tank_face_submit(&request) == ESP_OK
                   ? DIAGNOSTIC_OK : DIAGNOSTIC_UNAVAILABLE;
    }
    case DIAGNOSTIC_VOLUME:
        if (input[2] > 100U) return DIAGNOSTIC_INVALID;
        return baby_tank_audio_io_set_volume(input[2]) == ESP_OK
                   ? DIAGNOSTIC_OK : DIAGNOSTIC_UNAVAILABLE;
    case DIAGNOSTIC_GAME_RESULT:
        if (input[2] > 2U) return DIAGNOSTIC_INVALID;
        return start_tone((uint8_t)(7U + input[2]));
    default:
        return DIAGNOSTIC_INVALID;
    }
}

static void fill_snapshot(uint8_t *response, diagnostic_status_t status,
                          uint8_t opcode)
{
    response[0] = BABY_TANK_DIAGNOSTICS_VERSION;
    response[1] = opcode | 0x80U;
    response[2] = (uint8_t)status;
    uint8_t flags = 0U;
    if (baby_tank_network_is_connected()) flags |= 1U << 0;
    if (baby_tank_network_cloud_is_ready()) flags |= 1U << 1;
    if (baby_tank_network_softap_is_ready()) flags |= 1U << 2;
    if (baby_tank_camera_stream_is_ready()) flags |= 1U << 3;
    if (baby_tank_chassis_bridge_ready()) flags |= 1U << 4;
    if (baby_tank_chassis_bridge_link_verified()) flags |= 1U << 5;
    if (baby_tank_audio_io_is_playing()) flags |= 1U << 6;
    if (baby_tank_touch_wake_ready()) flags |= 1U << 7;
    response[3] = flags;

    baby_tank_face_snapshot_t face = {0};
    if (baby_tank_face_get_snapshot(&face) == ESP_OK) {
        response[4] = (uint8_t)face.expression;
        response[5] = face.intensity;
        response[6] = face.speech_level;
    }
    baby_tank_conversation_snapshot_t conversation = {0};
    if (baby_tank_conversation_get_snapshot(&conversation) == ESP_OK) {
        response[7] = (uint8_t)conversation.phase;
    }
    baby_tank_touch_snapshot_t touch = {0};
    if (baby_tank_touch_wake_get_snapshot(&touch) == ESP_OK) {
        response[8] = touch.active_mask;
        for (size_t index = 0; index < 3U; ++index) {
            uint32_t count = touch.trigger_count[index];
            if (count > UINT16_MAX) count = UINT16_MAX;
            write_u16(response + 20U + index * 2U, (uint16_t)count);
            write_u16(response + 26U + index * 2U,
                      touch.delta_per_mille[index]);
            write_u16(response + 32U + index * 2U,
                      touch.threshold_per_mille[index]);
        }
    }
    response[9] = (uint8_t)baby_tank_network_internet_phase();
    response[10] = (uint8_t)baby_tank_network_web_wifi_phase();
    response[11] = baby_tank_voice_runtime_remote_mode() ? 1U : 0U;
    response[38] = baby_tank_audio_io_get_volume();
    write_u32(response + 12U,
              (uint32_t)(esp_timer_get_time() / 1000000LL));
    write_u32(response + 16U,
              (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

esp_err_t baby_tank_diagnostics_endpoint(uint32_t session_id,
                                         const uint8_t *input,
                                         ssize_t input_size,
                                         uint8_t **output,
                                         ssize_t *output_size,
                                         void *context)
{
    (void)session_id;
    (void)context;
    if (input == NULL || input_size != BABY_TANK_DIAGNOSTICS_REQUEST_SIZE ||
        input[0] != BABY_TANK_DIAGNOSTICS_VERSION || output == NULL ||
        output_size == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t *response = calloc(1, BABY_TANK_DIAGNOSTICS_RESPONSE_SIZE);
    if (response == NULL) return ESP_ERR_NO_MEM;
    diagnostic_status_t status = run_command(input);
    fill_snapshot(response, status, input[1]);
    *output = response;
    *output_size = BABY_TANK_DIAGNOSTICS_RESPONSE_SIZE;
    return ESP_OK;
}
