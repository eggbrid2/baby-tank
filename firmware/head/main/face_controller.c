#include "face_controller.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "conversation.h"

#define FACE_FRAME_INTERVAL_MS 50U
#define FACE_REQUEST_QUEUE_LENGTH 8U
#define FACE_TASK_STACK_SIZE 4096U
#define FACE_TASK_PRIORITY 5U
#define FACE_MIN_DURATION_MS 1200U
#define FACE_MAX_DURATION_MS 30000U
#define FACE_MIN_INTENSITY 20U
#define FACE_MAX_INTENSITY 100U
#define FACE_NORMAL_IDLE_MS 60000U
#define FACE_BORED_IDLE_MS 30000U

static const char *TAG = "baby_tank_face";

static QueueHandle_t s_request_queue;
static SemaphoreHandle_t s_snapshot_mutex;
static SemaphoreHandle_t s_drawing_mutex;
static baby_tank_drawing_t s_drawing;
static TickType_t s_drawing_until;
static TaskHandle_t s_face_task;
static baby_tank_face_snapshot_t s_snapshot;
static portMUX_TYPE s_speech_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_speech_target;
static portMUX_TYPE s_idle_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_forced_standby;
static TickType_t s_last_activity_tick;
static baby_tank_presence_state_t s_presence_state =
    BABY_TANK_PRESENCE_NORMAL;
static baby_tank_expression_t s_idle_expression = BABY_TANK_EXPRESSION_CALM;
static uint8_t s_idle_intensity = 45U;

static void read_idle_state(bool *forced_standby,
                            TickType_t *last_activity_tick,
                            baby_tank_expression_t *expression,
                            uint8_t *intensity)
{
    taskENTER_CRITICAL(&s_idle_lock);
    *forced_standby = s_forced_standby;
    *last_activity_tick = s_last_activity_tick;
    *expression = s_idle_expression;
    *intensity = s_idle_intensity;
    taskEXIT_CRITICAL(&s_idle_lock);
}

static baby_tank_presence_state_t presence_for_idle_time(
    TickType_t now, TickType_t last_activity_tick, bool forced_standby)
{
    if (forced_standby) return BABY_TANK_PRESENCE_SLEEPING;
    uint32_t idle_ms = (uint32_t)((now - last_activity_tick) *
                                  portTICK_PERIOD_MS);
    if (idle_ms < FACE_NORMAL_IDLE_MS) return BABY_TANK_PRESENCE_NORMAL;
    if (idle_ms < FACE_NORMAL_IDLE_MS + FACE_BORED_IDLE_MS) {
        return BABY_TANK_PRESENCE_BORED;
    }
    return BABY_TANK_PRESENCE_SLEEPING;
}

static void publish_presence_state(baby_tank_presence_state_t state)
{
    bool changed;
    taskENTER_CRITICAL(&s_idle_lock);
    changed = s_presence_state != state;
    s_presence_state = state;
    taskEXIT_CRITICAL(&s_idle_lock);
    if (changed) {
        static const char *const names[] = {"normal", "bored", "sleeping"};
        ESP_LOGI(TAG, "presence state=%s", names[state]);
    }
}

static uint32_t clamp_duration(uint32_t duration_ms)
{
    if (duration_ms < FACE_MIN_DURATION_MS) {
        return FACE_MIN_DURATION_MS;
    }
    if (duration_ms > FACE_MAX_DURATION_MS) {
        return FACE_MAX_DURATION_MS;
    }
    return duration_ms;
}

static uint8_t clamp_intensity(uint8_t intensity)
{
    if (intensity < FACE_MIN_INTENSITY) {
        return FACE_MIN_INTENSITY;
    }
    if (intensity > FACE_MAX_INTENSITY) {
        return FACE_MAX_INTENSITY;
    }
    return intensity;
}

static bool valid_priority(baby_tank_face_priority_t priority)
{
    return priority == BABY_TANK_FACE_PRIORITY_AMBIENT ||
           priority == BABY_TANK_FACE_PRIORITY_AI ||
           priority == BABY_TANK_FACE_PRIORITY_INTERACTION ||
           priority == BABY_TANK_FACE_PRIORITY_SAFETY;
}

static baby_tank_expression_t ambient_expression_for_mood(
    baby_tank_expression_t mood)
{
    static const baby_tank_expression_t positive[] = {
        BABY_TANK_EXPRESSION_HAPPY, BABY_TANK_EXPRESSION_CURIOUS,
        BABY_TANK_EXPRESSION_SHY, BABY_TANK_EXPRESSION_PROUD,
        BABY_TANK_EXPRESSION_LOVE,
    };
    static const baby_tank_expression_t low[] = {
        BABY_TANK_EXPRESSION_SAD, BABY_TANK_EXPRESSION_WORRIED,
        BABY_TANK_EXPRESSION_THINKING, BABY_TANK_EXPRESSION_SLEEPY,
    };
    static const baby_tank_expression_t tense[] = {
        BABY_TANK_EXPRESSION_ANNOYED, BABY_TANK_EXPRESSION_COMPLAINING,
        BABY_TANK_EXPRESSION_THINKING, BABY_TANK_EXPRESSION_CONFUSED,
    };
    static const baby_tank_expression_t curious[] = {
        BABY_TANK_EXPRESSION_CURIOUS, BABY_TANK_EXPRESSION_THINKING,
        BABY_TANK_EXPRESSION_CONFUSED, BABY_TANK_EXPRESSION_SURPRISED,
    };
    const baby_tank_expression_t *choices = positive;
    size_t count = sizeof(positive) / sizeof(positive[0]);
    switch (mood) {
    case BABY_TANK_EXPRESSION_SAD:
    case BABY_TANK_EXPRESSION_CRYING:
    case BABY_TANK_EXPRESSION_WORRIED:
    case BABY_TANK_EXPRESSION_SICK:
    case BABY_TANK_EXPRESSION_SLEEPY:
        choices = low;
        count = sizeof(low) / sizeof(low[0]);
        break;
    case BABY_TANK_EXPRESSION_ANNOYED:
    case BABY_TANK_EXPRESSION_COMPLAINING:
    case BABY_TANK_EXPRESSION_ANGRY:
    case BABY_TANK_EXPRESSION_FURIOUS:
        choices = tense;
        count = sizeof(tense) / sizeof(tense[0]);
        break;
    case BABY_TANK_EXPRESSION_CURIOUS:
    case BABY_TANK_EXPRESSION_CONFUSED:
    case BABY_TANK_EXPRESSION_THINKING:
        choices = curious;
        count = sizeof(curious) / sizeof(curious[0]);
        break;
    default:
        break;
    }
    return choices[esp_random() % count];
}

static void publish_snapshot(const baby_tank_face_request_t *active,
                             uint32_t elapsed_ms,
                             uint8_t speech_level)
{
    if (xSemaphoreTake(s_snapshot_mutex, 0) != pdTRUE) {
        return;
    }
    s_snapshot.expression = active->expression;
    s_snapshot.elapsed_ms = elapsed_ms;
    s_snapshot.duration_ms = active->duration_ms;
    s_snapshot.intensity = active->intensity;
    s_snapshot.speech_level = speech_level;
    s_snapshot.priority = active->priority;
    xSemaphoreGive(s_snapshot_mutex);
}

static void activate_request(baby_tank_face_request_t *active,
                             TickType_t *active_start,
                             const baby_tank_face_request_t *next,
                             TickType_t now)
{
    *active = *next;
    *active_start = now;
    ESP_LOGI(TAG,
             "active emotion=%s intensity=%u duration_ms=%" PRIu32 " priority=%u",
             baby_tank_expression_name(active->expression),
             active->intensity,
             active->duration_ms,
             (unsigned)active->priority);
}

static void face_task(void *context)
{
    (void)context;
    baby_tank_face_request_t active = {
        .expression = BABY_TANK_EXPRESSION_SLEEPING,
        .duration_ms = UINT32_MAX,
        .intensity = 55U,
        .priority = BABY_TANK_FACE_PRIORITY_IDLE,
    };
    TickType_t active_start = xTaskGetTickCount();
    TickType_t frame_wake = active_start;
    TickType_t next_ambient = active_start + pdMS_TO_TICKS(20000U);
    float speech_level = 0.0f;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed_ms =
            (uint32_t)((now - active_start) * portTICK_PERIOD_MS);
        bool expired = active.duration_ms != UINT32_MAX &&
                       elapsed_ms >= active.duration_ms;
        bool forced_standby;
        TickType_t last_activity_tick;
        baby_tank_expression_t idle_expression;
        uint8_t idle_intensity;
        read_idle_state(&forced_standby, &last_activity_tick,
                        &idle_expression, &idle_intensity);
        baby_tank_conversation_snapshot_t dialogue = {0};
        bool dialogue_awake =
            baby_tank_conversation_ready() &&
            baby_tank_conversation_get_snapshot(&dialogue) == ESP_OK &&
            dialogue.phase != BABY_TANK_CONVERSATION_IDLE &&
            dialogue.phase != BABY_TANK_CONVERSATION_CANCELLED &&
            dialogue.phase != BABY_TANK_CONVERSATION_ERROR &&
            dialogue.phase != BABY_TANK_CONVERSATION_DISCONNECTED;
        baby_tank_presence_state_t presence = presence_for_idle_time(
            now, last_activity_tick, forced_standby);
        /* Conversation state may be changed by cloud/network maintenance.
         * It can keep an already-awake face in NORMAL, but it must never wake
         * a sleeping face. Real touch/voice/app entry points reset the
         * activity timer explicitly before they change conversation phase. */
        if (dialogue_awake && presence != BABY_TANK_PRESENCE_SLEEPING) {
            presence = BABY_TANK_PRESENCE_NORMAL;
        }
        publish_presence_state(presence);
        bool standby = presence == BABY_TANK_PRESENCE_SLEEPING;
        baby_tank_expression_t desired_idle = standby
            ? BABY_TANK_EXPRESSION_SLEEPING
            : presence == BABY_TANK_PRESENCE_BORED
                ? BABY_TANK_EXPRESSION_BORED : idle_expression;
        if (presence == BABY_TANK_PRESENCE_BORED) idle_intensity = 62U;
        if (dialogue_awake && desired_idle == BABY_TANK_EXPRESSION_SLEEPY) {
            desired_idle = BABY_TANK_EXPRESSION_CURIOUS;
            idle_intensity = 68U;
        }
        if (expired) {
            bool returning_to_idle = active.priority != BABY_TANK_FACE_PRIORITY_IDLE;
            baby_tank_face_request_t idle = {
                .expression = desired_idle,
                .duration_ms = UINT32_MAX,
                .intensity = standby ? 55U : idle_intensity,
                .priority = BABY_TANK_FACE_PRIORITY_IDLE,
            };
            activate_request(&active, &active_start, &idle, now);
            elapsed_ms = 0;
            if (returning_to_idle) {
                next_ambient = now + pdMS_TO_TICKS(18000U + esp_random() % 18000U);
            }
        }

        if (active.priority == BABY_TANK_FACE_PRIORITY_IDLE &&
            (active.expression != desired_idle ||
             active.intensity != (standby ? 55U : idle_intensity))) {
            baby_tank_face_request_t idle = {
                .expression = desired_idle,
                .duration_ms = UINT32_MAX,
                .intensity = standby ? 55U : idle_intensity,
                .priority = BABY_TANK_FACE_PRIORITY_IDLE,
            };
            activate_request(&active, &active_start, &idle, now);
            elapsed_ms = 0U;
        }

        baby_tank_face_request_t selected;
        bool has_selected = false;
        baby_tank_face_request_t candidate;
        while (xQueueReceive(s_request_queue, &candidate, 0) == pdTRUE) {
            if (!has_selected || candidate.priority >= selected.priority) {
                selected = candidate;
                has_selected = true;
            }
        }
        if (has_selected) {
            if (selected.priority >= active.priority ||
                active.priority == BABY_TANK_FACE_PRIORITY_IDLE) {
                activate_request(&active, &active_start, &selected, now);
                elapsed_ms = 0;
            } else {
                ESP_LOGI(TAG,
                         "ignored emotion=%s priority=%u; active priority=%u",
                         baby_tank_expression_name(selected.expression),
                         (unsigned)selected.priority,
                         (unsigned)active.priority);
            }
        }

        if (presence == BABY_TANK_PRESENCE_NORMAL && !has_selected &&
            active.priority == BABY_TANK_FACE_PRIORITY_IDLE &&
            (int32_t)(now - next_ambient) >= 0) {
            dialogue = (baby_tank_conversation_snapshot_t){0};
            if (baby_tank_conversation_get_snapshot(&dialogue) != ESP_OK ||
                dialogue.phase != BABY_TANK_CONVERSATION_IDLE) {
                next_ambient = now + pdMS_TO_TICKS(5000U);
            } else {
                baby_tank_face_request_t ambient = {
                    .expression = ambient_expression_for_mood(idle_expression),
                    .duration_ms = 2200U,
                    .intensity = 72U,
                    .priority = BABY_TANK_FACE_PRIORITY_AMBIENT,
                };
                activate_request(&active, &active_start, &ambient, now);
                elapsed_ms = 0;
            }
        }

        taskENTER_CRITICAL(&s_speech_lock);
        uint8_t speech_target = s_speech_target;
        taskEXIT_CRITICAL(&s_speech_lock);
        float speech_rate = speech_target > speech_level ? 0.58f : 0.30f;
        speech_level += ((float)speech_target - speech_level) * speech_rate;
        uint8_t rendered_speech_level = (uint8_t)(speech_level + 0.5f);

        bool show_drawing = false;
        esp_err_t render_result = ESP_OK;
        if (s_drawing_mutex != NULL &&
            xSemaphoreTake(s_drawing_mutex, 0) == pdTRUE) {
            bool drawing_expired = s_drawing.pixels != NULL &&
                                   (int32_t)(s_drawing_until - now) <= 0;
            if (drawing_expired) {
                heap_caps_free(s_drawing.pixels);
                memset(&s_drawing, 0, sizeof(s_drawing));
                s_drawing_until = 0U;
            }
            show_drawing = s_drawing.pixels != NULL &&
                           (int32_t)(s_drawing_until - now) > 0 &&
                           active.priority != BABY_TANK_FACE_PRIORITY_SAFETY;
            if (show_drawing) {
                render_result = baby_tank_display_render_drawing(&s_drawing);
            }
            xSemaphoreGive(s_drawing_mutex);
        }
        if (!show_drawing) {
            render_result = baby_tank_display_render_expression(
                active.expression, elapsed_ms, active.duration_ms,
                active.intensity, rendered_speech_level);
        }
        if (render_result != ESP_OK) {
            ESP_LOGE(TAG, "render failed: %s", esp_err_to_name(render_result));
        }
        publish_snapshot(&active, elapsed_ms, rendered_speech_level);
        vTaskDelayUntil(&frame_wake, pdMS_TO_TICKS(FACE_FRAME_INTERVAL_MS));
    }
}

esp_err_t baby_tank_face_controller_start(void)
{
    ESP_RETURN_ON_FALSE(s_face_task == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "face controller already started");
    ESP_RETURN_ON_ERROR(baby_tank_display_init(), TAG, "display initialization failed");

    s_request_queue = xQueueCreate(FACE_REQUEST_QUEUE_LENGTH,
                                   sizeof(baby_tank_face_request_t));
    ESP_RETURN_ON_FALSE(s_request_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to allocate face request queue");
    s_snapshot_mutex = xSemaphoreCreateMutex();
    s_drawing_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_snapshot_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to allocate face snapshot mutex");
    ESP_RETURN_ON_FALSE(s_drawing_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to allocate drawing mutex");

    taskENTER_CRITICAL(&s_idle_lock);
    s_forced_standby = false;
    s_last_activity_tick = xTaskGetTickCount();
    s_presence_state = BABY_TANK_PRESENCE_NORMAL;
    taskEXIT_CRITICAL(&s_idle_lock);

    BaseType_t task_result = xTaskCreate(face_task,
                                         "baby_tank_face",
                                         FACE_TASK_STACK_SIZE,
                                         NULL,
                                         FACE_TASK_PRIORITY,
                                         &s_face_task);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "unable to create face task");
    ESP_LOGI(TAG, "AI-ready face controller started");
    return ESP_OK;
}

esp_err_t baby_tank_face_submit(const baby_tank_face_request_t *request)
{
    ESP_RETURN_ON_FALSE(request != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "request is required");
    ESP_RETURN_ON_FALSE(s_request_queue != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "face controller is not started");
    ESP_RETURN_ON_FALSE(request->expression >= 0 &&
                            request->expression < BABY_TANK_EXPRESSION_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid expression");
    ESP_RETURN_ON_FALSE(valid_priority(request->priority), ESP_ERR_INVALID_ARG,
                        TAG, "invalid priority");

    baby_tank_face_request_t sanitized = *request;
    if (sanitized.duration_ms != BABY_TANK_FACE_DURATION_HOLD) {
        sanitized.duration_ms = clamp_duration(sanitized.duration_ms);
    }
    sanitized.intensity = clamp_intensity(sanitized.intensity);
    ESP_RETURN_ON_FALSE(xQueueSend(s_request_queue, &sanitized, 0) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "face request queue is full");
    return ESP_OK;
}

esp_err_t baby_tank_face_submit_ai(const char *emotion,
                                   uint8_t intensity,
                                   uint32_t duration_ms)
{
    baby_tank_expression_t expression = BABY_TANK_EXPRESSION_COUNT;
    ESP_RETURN_ON_ERROR(baby_tank_face_expression_from_name(emotion, &expression),
                        TAG, "unsupported AI emotion");

    const baby_tank_face_request_t request = {
        .expression = expression,
        .duration_ms = clamp_duration(duration_ms),
        .intensity = intensity,
        .priority = BABY_TANK_FACE_PRIORITY_AI,
    };
    return baby_tank_face_submit(&request);
}

esp_err_t baby_tank_face_set_mood(const char *emotion, uint8_t intensity)
{
    baby_tank_expression_t expression = BABY_TANK_EXPRESSION_COUNT;
    ESP_RETURN_ON_ERROR(baby_tank_face_expression_from_name(emotion, &expression),
                        TAG, "unsupported mood");
    ESP_RETURN_ON_FALSE(expression != BABY_TANK_EXPRESSION_SLEEPING &&
                            expression != BABY_TANK_EXPRESSION_BUBBLE &&
                            expression != BABY_TANK_EXPRESSION_CANNON &&
                            expression != BABY_TANK_EXPRESSION_LISTENING &&
                            expression != BABY_TANK_EXPRESSION_PROCESSING &&
                            expression < BABY_TANK_EXPRESSION_EATING,
                        ESP_ERR_INVALID_ARG, TAG,
                        "transient expression cannot be an idle mood");
    taskENTER_CRITICAL(&s_idle_lock);
    s_idle_expression = expression;
    s_idle_intensity = clamp_intensity(intensity);
    taskEXIT_CRITICAL(&s_idle_lock);
    return ESP_OK;
}

esp_err_t baby_tank_face_set_standby(bool standby)
{
    ESP_RETURN_ON_FALSE(s_face_task != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "face controller is not started");
    taskENTER_CRITICAL(&s_idle_lock);
    baby_tank_presence_state_t previous_state = s_presence_state;
    bool was_forced_standby = s_forced_standby;
    s_forced_standby = standby;
    if (!standby) {
        s_last_activity_tick = xTaskGetTickCount();
    }
    taskEXIT_CRITICAL(&s_idle_lock);
    if (!standby &&
        (was_forced_standby ||
         previous_state != BABY_TANK_PRESENCE_NORMAL)) {
        static const char *const names[] = {"normal", "bored", "sleeping"};
        ESP_LOGI(TAG, "explicit activity wakes from state=%s",
                 names[previous_state]);
        ESP_LOGI(TAG, "wake source task=%s", pcTaskGetName(NULL));
    }
    return ESP_OK;
}

esp_err_t baby_tank_face_note_activity(void)
{
    return baby_tank_face_set_standby(false);
}

esp_err_t baby_tank_face_get_presence_state(
    baby_tank_presence_state_t *state)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "presence state output is required");
    ESP_RETURN_ON_FALSE(s_face_task != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "face controller is not started");
    taskENTER_CRITICAL(&s_idle_lock);
    *state = s_presence_state;
    taskEXIT_CRITICAL(&s_idle_lock);
    return ESP_OK;
}

esp_err_t baby_tank_face_set_thought(const char *thought)
{
    ESP_RETURN_ON_FALSE(thought != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "thought key is required");
    static const struct {
        const char *name;
        baby_tank_thought_t thought;
    } thoughts[] = {
        {"star", BABY_TANK_THOUGHT_STAR}, {"moon", BABY_TANK_THOUGHT_MOON},
        {"cloud", BABY_TANK_THOUGHT_CLOUD}, {"heart", BABY_TANK_THOUGHT_HEART},
        {"flower", BABY_TANK_THOUGHT_FLOWER}, {"rocket", BABY_TANK_THOUGHT_ROCKET},
        {"car", BABY_TANK_THOUGHT_CAR}, {"ball", BABY_TANK_THOUGHT_BALL},
        {"fish", BABY_TANK_THOUGHT_FISH}, {"bubble", BABY_TANK_THOUGHT_BUBBLE},
        {"question", BABY_TANK_THOUGHT_QUESTION}, {"music", BABY_TANK_THOUGHT_MUSIC},
    };
    for (size_t index = 0; index < sizeof(thoughts) / sizeof(thoughts[0]); ++index) {
        if (strcmp(thought, thoughts[index].name) == 0) {
            return baby_tank_display_set_sleep_thought(thoughts[index].thought);
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t baby_tank_face_set_thought_text(const char *utf8_text)
{
    return baby_tank_display_set_sleep_text(utf8_text);
}

esp_err_t baby_tank_face_submit_drawing(const baby_tank_drawing_t *drawing)
{
    ESP_RETURN_ON_FALSE(drawing != NULL && drawing->pixels != NULL &&
                            drawing->width == BABY_TANK_DRAWING_WIDTH &&
                            drawing->height == BABY_TANK_DRAWING_HEIGHT &&
                            drawing->pixel_count ==
                                BABY_TANK_DRAWING_WIDTH * BABY_TANK_DRAWING_HEIGHT &&
                            drawing->duration_ms >= 4000U &&
                            drawing->duration_ms <= 8000U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid drawing");
    ESP_RETURN_ON_FALSE(s_drawing_mutex != NULL &&
                            xSemaphoreTake(s_drawing_mutex,
                                pdMS_TO_TICKS(50)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "drawing busy");
    if (s_drawing.pixels != NULL) heap_caps_free(s_drawing.pixels);
    s_drawing = *drawing;
    s_drawing_until = xTaskGetTickCount() +
        pdMS_TO_TICKS(drawing->duration_ms);
    xSemaphoreGive(s_drawing_mutex);
    ESP_LOGI(TAG, "AI SVG drawing active pixels=%zu duration_ms=%u",
             drawing->pixel_count, (unsigned)drawing->duration_ms);
    return ESP_OK;
}

void baby_tank_face_cancel_drawing(void)
{
    if (s_drawing_mutex != NULL &&
        xSemaphoreTake(s_drawing_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_drawing.pixels != NULL) heap_caps_free(s_drawing.pixels);
        memset(&s_drawing, 0, sizeof(s_drawing));
        s_drawing_until = 0U;
        xSemaphoreGive(s_drawing_mutex);
    }
}

esp_err_t baby_tank_face_expression_from_name(const char *emotion,
                                              baby_tank_expression_t *expression)
{
    ESP_RETURN_ON_FALSE(emotion != NULL && expression != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "emotion and output are required");
    ESP_RETURN_ON_FALSE(strnlen(emotion, 33U) <= 32U, ESP_ERR_INVALID_ARG, TAG,
                        "emotion name is too long");
    for (int index = 0; index < BABY_TANK_EXPRESSION_COUNT; ++index) {
        baby_tank_expression_t candidate = (baby_tank_expression_t)index;
        if (strcmp(emotion, baby_tank_expression_name(candidate)) == 0) {
            *expression = candidate;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t baby_tank_face_set_speech_level(uint8_t level)
{
    ESP_RETURN_ON_FALSE(s_face_task != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "face controller is not started");
    if (level > 100U) {
        level = 100U;
    }
    taskENTER_CRITICAL(&s_speech_lock);
    s_speech_target = level;
    taskEXIT_CRITICAL(&s_speech_lock);
    return ESP_OK;
}

esp_err_t baby_tank_face_get_snapshot(baby_tank_face_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "snapshot is required");
    ESP_RETURN_ON_FALSE(s_snapshot_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "face controller is not started");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(20)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "face snapshot is busy");
    *snapshot = s_snapshot;
    xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}
