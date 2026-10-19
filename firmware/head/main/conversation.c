#include "conversation.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "face_controller.h"

#define CONVERSATION_LOCK_MS 20U
#define CONVERSATION_ERROR_DURATION_MS 5000U
#define CONVERSATION_REPLY_EXPRESSION_HOLD_MS 3000U
#define CONVERSATION_MAX_PCM_SAMPLES 4096U
#define CONVERSATION_PCM_NOISE_FLOOR 250U
#define CONVERSATION_PCM_FULL_SCALE 7000U

static const char *TAG = "baby_tank_conversation";

static SemaphoreHandle_t s_mutex;
static baby_tank_conversation_snapshot_t s_snapshot;
static baby_tank_expression_t s_ai_expression = BABY_TANK_EXPRESSION_CALM;
static uint8_t s_ai_intensity = 65U;

static bool valid_phase(baby_tank_conversation_phase_t phase)
{
    return phase >= BABY_TANK_CONVERSATION_IDLE &&
           phase <= BABY_TANK_CONVERSATION_DISCONNECTED;
}

static uint8_t clamp_intensity(uint8_t intensity)
{
    if (intensity < 20U) return 20U;
    if (intensity > 100U) return 100U;
    return intensity;
}

static void phase_face(baby_tank_conversation_phase_t phase,
                       baby_tank_expression_t ai_expression,
                       uint8_t ai_intensity,
                       baby_tank_expression_t *expression,
                       uint8_t *intensity,
                       uint32_t *duration_ms)
{
    *duration_ms = BABY_TANK_FACE_DURATION_HOLD;
    switch (phase) {
    case BABY_TANK_CONVERSATION_IDLE:
        *expression = BABY_TANK_EXPRESSION_CALM;
        *intensity = 45U;
        *duration_ms = 1200U;
        break;
    case BABY_TANK_CONVERSATION_LISTENING:
        *expression = BABY_TANK_EXPRESSION_LISTENING;
        *intensity = 58U;
        break;
    case BABY_TANK_CONVERSATION_USER_SPEAKING:
        *expression = BABY_TANK_EXPRESSION_LISTENING;
        *intensity = 72U;
        break;
    case BABY_TANK_CONVERSATION_RECOGNIZING:
        *expression = BABY_TANK_EXPRESSION_PROCESSING;
        *intensity = 52U;
        break;
    case BABY_TANK_CONVERSATION_THINKING:
        *expression = BABY_TANK_EXPRESSION_PROCESSING;
        *intensity = 72U;
        break;
    case BABY_TANK_CONVERSATION_AI_SPEAKING:
        *expression = ai_expression;
        *intensity = ai_intensity;
        break;
    case BABY_TANK_CONVERSATION_CANCELLED:
        *expression = BABY_TANK_EXPRESSION_CALM;
        *intensity = 35U;
        *duration_ms = 1200U;
        break;
    case BABY_TANK_CONVERSATION_ERROR:
        *expression = BABY_TANK_EXPRESSION_WORRIED;
        *intensity = 82U;
        *duration_ms = CONVERSATION_ERROR_DURATION_MS;
        break;
    case BABY_TANK_CONVERSATION_DISCONNECTED:
        *expression = BABY_TANK_EXPRESSION_SAD;
        *intensity = 58U;
        break;
    }
}

static esp_err_t submit_phase_face(baby_tank_conversation_phase_t phase,
                                   baby_tank_expression_t expression,
                                   uint8_t intensity,
                                   uint32_t duration_ms)
{
    const baby_tank_face_request_t request = {
        .expression = expression,
        .duration_ms = duration_ms,
        .intensity = intensity,
        .priority = BABY_TANK_FACE_PRIORITY_INTERACTION,
    };
    esp_err_t result = baby_tank_face_submit(&request);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "phase=%s expression=%s intensity=%u",
                 baby_tank_conversation_phase_name(phase),
                 baby_tank_expression_name(expression), intensity);
    }
    return result;
}

esp_err_t baby_tank_conversation_init(void)
{
    ESP_RETURN_ON_FALSE(s_mutex == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "conversation adapter already initialized");
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to allocate conversation mutex");
    s_snapshot = (baby_tank_conversation_snapshot_t) {
        .phase = BABY_TANK_CONVERSATION_IDLE,
        .expression = BABY_TANK_EXPRESSION_CALM,
        .intensity = 45U,
        .speech_level = 0U,
        .turn_id = 0U,
    };
    ESP_RETURN_ON_ERROR(baby_tank_face_set_speech_level(0U), TAG,
                        "unable to reset speech envelope");
    ESP_LOGI(TAG, "provider-independent conversation adapter ready");
    return ESP_OK;
}

bool baby_tank_conversation_ready(void)
{
    return s_mutex != NULL;
}

esp_err_t baby_tank_conversation_set_phase(baby_tank_conversation_phase_t phase)
{
    ESP_RETURN_ON_FALSE(valid_phase(phase), ESP_ERR_INVALID_ARG, TAG,
                        "invalid conversation phase");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "conversation adapter is not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mutex,
                                      pdMS_TO_TICKS(CONVERSATION_LOCK_MS)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "conversation state is busy");

    const baby_tank_conversation_phase_t previous_phase = s_snapshot.phase;
    bool retain_reply_expression =
        phase == BABY_TANK_CONVERSATION_IDLE &&
        s_snapshot.phase == BABY_TANK_CONVERSATION_AI_SPEAKING;
    baby_tank_expression_t retained_expression = s_ai_expression;
    uint8_t retained_intensity = s_ai_intensity;
    if (phase == BABY_TANK_CONVERSATION_LISTENING &&
        s_snapshot.phase != BABY_TANK_CONVERSATION_LISTENING) {
        ++s_snapshot.turn_id;
        s_ai_expression = BABY_TANK_EXPRESSION_CALM;
        s_ai_intensity = 65U;
    }
    baby_tank_expression_t expression;
    uint8_t intensity;
    uint32_t duration_ms;
    phase_face(phase, s_ai_expression, s_ai_intensity,
               &expression, &intensity, &duration_ms);
    s_snapshot.phase = phase;
    s_snapshot.expression = expression;
    s_snapshot.intensity = intensity;
    if (phase != BABY_TANK_CONVERSATION_AI_SPEAKING) {
        s_snapshot.speech_level = 0U;
    }
    esp_err_t result = ESP_OK;
    if (phase != BABY_TANK_CONVERSATION_AI_SPEAKING) {
        result = baby_tank_face_set_speech_level(0U);
    }
    if (result == ESP_OK && retain_reply_expression) {
        const baby_tank_face_request_t request = {
            .expression = retained_expression,
            .duration_ms = CONVERSATION_REPLY_EXPRESSION_HOLD_MS,
            .intensity = retained_intensity,
            .priority = BABY_TANK_FACE_PRIORITY_INTERACTION,
        };
        result = baby_tank_face_submit(&request);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "reply expression retained=%s duration=%u ms",
                     baby_tank_expression_name(request.expression),
                     (unsigned)request.duration_ms);
        }
    } else if (result == ESP_OK) {
        result = submit_phase_face(phase, expression, intensity, duration_ms);
    }
    const uint32_t transition_turn_id = s_snapshot.turn_id;
    xSemaphoreGive(s_mutex);
    if (previous_phase != phase) {
        ESP_LOGI(TAG, "phase transition %s -> %s turn=%lu",
                 baby_tank_conversation_phase_name(previous_phase),
                 baby_tank_conversation_phase_name(phase),
                 (unsigned long)transition_turn_id);
    }
    return result;
}

esp_err_t baby_tank_conversation_resume_listening(void)
{
    /* Multi-turn listening has its own persistent visual state. */
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "conversation adapter is not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mutex,
                                      pdMS_TO_TICKS(CONVERSATION_LOCK_MS)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "conversation state is busy");
    if (s_snapshot.phase != BABY_TANK_CONVERSATION_IDLE) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot.phase = BABY_TANK_CONVERSATION_LISTENING;
    s_snapshot.expression = BABY_TANK_EXPRESSION_LISTENING;
    s_snapshot.intensity = 58U;
    s_snapshot.speech_level = 0U;
    ++s_snapshot.turn_id;
    xSemaphoreGive(s_mutex);
    ESP_RETURN_ON_ERROR(baby_tank_face_set_speech_level(0U), TAG,
                        "unable to reset speech envelope");
    ESP_LOGI(TAG, "listening resumed with dedicated ear animation");
    return submit_phase_face(BABY_TANK_CONVERSATION_LISTENING,
                             BABY_TANK_EXPRESSION_LISTENING, 58U,
                             BABY_TANK_FACE_DURATION_HOLD);
}

esp_err_t baby_tank_conversation_set_ai_emotion(const char *emotion,
                                                uint8_t intensity)
{
    baby_tank_expression_t expression;
    ESP_RETURN_ON_ERROR(baby_tank_face_expression_from_name(emotion, &expression),
                        TAG, "unsupported model emotion");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "conversation adapter is not initialized");
    intensity = clamp_intensity(intensity);
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mutex,
                                      pdMS_TO_TICKS(CONVERSATION_LOCK_MS)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "conversation state is busy");
    s_ai_expression = expression;
    s_ai_intensity = intensity;
    bool speaking = s_snapshot.phase == BABY_TANK_CONVERSATION_AI_SPEAKING;
    if (speaking) {
        s_snapshot.expression = expression;
        s_snapshot.intensity = intensity;
    }
    xSemaphoreGive(s_mutex);

    if (!speaking) return ESP_OK;
    return submit_phase_face(BABY_TANK_CONVERSATION_AI_SPEAKING,
                             expression, intensity,
                             BABY_TANK_FACE_DURATION_HOLD);
}

esp_err_t baby_tank_conversation_observe_tts_pcm16(const int16_t *samples,
                                                   size_t sample_count)
{
    ESP_RETURN_ON_FALSE(samples != NULL && sample_count > 0U,
                        ESP_ERR_INVALID_ARG, TAG, "PCM samples are required");
    ESP_RETURN_ON_FALSE(sample_count <= CONVERSATION_MAX_PCM_SAMPLES,
                        ESP_ERR_INVALID_SIZE, TAG, "PCM chunk is too large");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "conversation adapter is not initialized");

    uint64_t absolute_sum = 0U;
    for (size_t index = 0; index < sample_count; ++index) {
        int32_t sample = samples[index];
        absolute_sum += (uint32_t)(sample < 0 ? -sample : sample);
    }
    uint32_t average = (uint32_t)(absolute_sum / sample_count);
    uint8_t level = 0U;
    if (average > CONVERSATION_PCM_NOISE_FLOOR) {
        uint32_t scaled = (average - CONVERSATION_PCM_NOISE_FLOOR) * 100U /
                          (CONVERSATION_PCM_FULL_SCALE -
                           CONVERSATION_PCM_NOISE_FLOOR);
        level = scaled > 100U ? 100U : (uint8_t)scaled;
    }

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mutex,
                                      pdMS_TO_TICKS(CONVERSATION_LOCK_MS)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "conversation state is busy");
    bool speaking = s_snapshot.phase == BABY_TANK_CONVERSATION_AI_SPEAKING;
    if (speaking) s_snapshot.speech_level = level;
    xSemaphoreGive(s_mutex);
    ESP_RETURN_ON_FALSE(speaking, ESP_ERR_INVALID_STATE, TAG,
                        "PCM observed outside AI speaking phase");
    return baby_tank_face_set_speech_level(level);
}

esp_err_t baby_tank_conversation_get_snapshot(
    baby_tank_conversation_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "snapshot is required");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "conversation adapter is not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mutex,
                                      pdMS_TO_TICKS(CONVERSATION_LOCK_MS)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "conversation state is busy");
    *snapshot = s_snapshot;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

const char *baby_tank_conversation_phase_name(
    baby_tank_conversation_phase_t phase)
{
    static const char *const names[] = {
        "idle", "listening", "user_speaking", "recognizing", "thinking",
        "ai_speaking", "cancelled", "error", "disconnected",
    };
    return valid_phase(phase) ? names[phase] : "unknown";
}
