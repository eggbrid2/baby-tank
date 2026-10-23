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
#include "runtime_policy.h"
#include "state_event_queue.h"
#include "voice_runtime.h"

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
#define FACE_TIME_BAND_FRESH_MS (15U * 60U * 1000U)
#define FACE_DAY_SCENE_DURATION_MS (60U * 60U * 1000U)

typedef enum {
    FACE_BAND_UNKNOWN = 0,
    FACE_BAND_MORNING,
    FACE_BAND_MIDDAY,
    FACE_BAND_AFTERNOON,
    FACE_BAND_EARLY_EVENING,
    FACE_BAND_EVENING,
    FACE_BAND_LATE_NIGHT,
} face_time_band_t;

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
static face_time_band_t s_time_band = FACE_BAND_UNKNOWN;
static TickType_t s_time_band_updated;
static baby_tank_scene_t s_preview_scene = BABY_TANK_SCENE_COUNT;
static TickType_t s_preview_started;
static TickType_t s_preview_until;

static bool read_scene_preview(TickType_t now, baby_tank_scene_t *scene,
                               TickType_t *started)
{
    taskENTER_CRITICAL(&s_idle_lock);
    bool active = s_preview_scene < BABY_TANK_SCENE_COUNT &&
                  (int32_t)(s_preview_until - now) > 0;
    if (active) {
        *scene = s_preview_scene;
        *started = s_preview_started;
    } else {
        s_preview_scene = BABY_TANK_SCENE_COUNT;
        s_preview_until = 0U;
    }
    taskEXIT_CRITICAL(&s_idle_lock);
    return active;
}

static void read_idle_state(bool *forced_standby,
                            TickType_t *last_activity_tick,
                            baby_tank_expression_t *expression,
                            uint8_t *intensity,
                            face_time_band_t *time_band,
                            TickType_t *time_band_updated)
{
    taskENTER_CRITICAL(&s_idle_lock);
    *forced_standby = s_forced_standby;
    *last_activity_tick = s_last_activity_tick;
    *expression = s_idle_expression;
    *intensity = s_idle_intensity;
    *time_band = s_time_band;
    *time_band_updated = s_time_band_updated;
    taskEXIT_CRITICAL(&s_idle_lock);
}

static baby_tank_presence_state_t presence_for_idle_time(
    TickType_t now, TickType_t last_activity_tick, bool forced_standby,
    face_time_band_t band, TickType_t band_updated)
{
    if (baby_tank_policy_is_locked()) return BABY_TANK_PRESENCE_SLEEPING;
    if (forced_standby) return BABY_TANK_PRESENCE_SLEEPING;
    uint32_t idle_ms = (uint32_t)((now - last_activity_tick) *
                                  portTICK_PERIOD_MS);
    uint32_t normal_ms = FACE_NORMAL_IDLE_MS;
    uint32_t bored_ms = FACE_BORED_IDLE_MS;
    bool daytime = baby_tank_policy_is_daytime();
    bool band_fresh = band != FACE_BAND_UNKNOWN &&
        (uint32_t)((now - band_updated) * portTICK_PERIOD_MS) <
            FACE_TIME_BAND_FRESH_MS;
    if (band_fresh) {
        /* A cached cloud band can be stale across 06:00 while offline. The
         * trusted UTC policy schedule wins for daytime presentation. */
        daytime = daytime || band <= FACE_BAND_EARLY_EVENING;
        normal_ms = band == FACE_BAND_MIDDAY ||
                    band == FACE_BAND_EARLY_EVENING ? 180000U : 120000U;
        bored_ms = normal_ms == 180000U ? 300000U : 180000U;
    }
    if (idle_ms < normal_ms) return BABY_TANK_PRESENCE_NORMAL;
    /* A verified daytime band stays in BORED between pixel scenes; it must
     * not silently turn into night sleep after the idle threshold. */
    /* With no trustworthy wall clock, idleness alone must not be interpreted
     * as night. The owner chose an awake offline boot; stay in BORED until
     * time is known, rather than reverting to a sleeping face after 90 s. */
    if (daytime || !baby_tank_policy_time_known() ||
        idle_ms < normal_ms + bored_ms) {
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

typedef struct {
    baby_tank_face_request_t steps[3];
    uint8_t count;
    uint8_t variant;
} ambient_sequence_t;

static uint8_t s_last_ambient_variant = UINT8_MAX;

static ambient_sequence_t ambient_sequence_for_mood(
    baby_tank_expression_t mood)
{
    static const uint8_t patterns[12][3] = {
        {0, 1, 0}, {0, 2, 1}, {1, 0, 3}, {2, 0, 1},
        {0, 3, 2}, {3, 1, 0}, {1, 4, 0}, {4, 0, 2},
        {2, 1, 4}, {3, 0, 4}, {4, 2, 0}, {1, 3, 2},
    };
    static const baby_tank_expression_t calm[] = {
        BABY_TANK_EXPRESSION_CALM, BABY_TANK_EXPRESSION_CURIOUS,
        BABY_TANK_EXPRESSION_SHY, BABY_TANK_EXPRESSION_THINKING,
        BABY_TANK_EXPRESSION_HAPPY,
    };
    static const baby_tank_expression_t happy[] = {
        BABY_TANK_EXPRESSION_HAPPY, BABY_TANK_EXPRESSION_LOVE,
        BABY_TANK_EXPRESSION_PROUD, BABY_TANK_EXPRESSION_LAUGHING,
        BABY_TANK_EXPRESSION_SHY,
    };
    static const baby_tank_expression_t excited[] = {
        BABY_TANK_EXPRESSION_EXCITED, BABY_TANK_EXPRESSION_SURPRISED,
        BABY_TANK_EXPRESSION_LAUGHING, BABY_TANK_EXPRESSION_PROUD,
        BABY_TANK_EXPRESSION_HAPPY,
    };
    static const baby_tank_expression_t curious[] = {
        BABY_TANK_EXPRESSION_CURIOUS, BABY_TANK_EXPRESSION_THINKING,
        BABY_TANK_EXPRESSION_CONFUSED, BABY_TANK_EXPRESSION_SURPRISED,
        BABY_TANK_EXPRESSION_SHY,
    };
    static const baby_tank_expression_t sleepy[] = {
        BABY_TANK_EXPRESSION_SLEEPY, BABY_TANK_EXPRESSION_BORED,
        BABY_TANK_EXPRESSION_THINKING, BABY_TANK_EXPRESSION_WORRIED,
        BABY_TANK_EXPRESSION_CALM,
    };
    static const baby_tank_expression_t sad[] = {
        BABY_TANK_EXPRESSION_SAD, BABY_TANK_EXPRESSION_WORRIED,
        BABY_TANK_EXPRESSION_CRYING, BABY_TANK_EXPRESSION_CONFUSED,
        BABY_TANK_EXPRESSION_BORED,
    };
    static const baby_tank_expression_t worried[] = {
        BABY_TANK_EXPRESSION_WORRIED, BABY_TANK_EXPRESSION_CONFUSED,
        BABY_TANK_EXPRESSION_THINKING, BABY_TANK_EXPRESSION_SAD,
        BABY_TANK_EXPRESSION_SHY,
    };
    static const baby_tank_expression_t annoyed[] = {
        BABY_TANK_EXPRESSION_ANNOYED, BABY_TANK_EXPRESSION_COMPLAINING,
        BABY_TANK_EXPRESSION_ANGRY, BABY_TANK_EXPRESSION_CONFUSED,
        BABY_TANK_EXPRESSION_FURIOUS,
    };
    const baby_tank_expression_t *pool = calm;
    uint8_t base_intensity = 54U;
    switch (mood) {
    case BABY_TANK_EXPRESSION_HAPPY:
    case BABY_TANK_EXPRESSION_LAUGHING:
    case BABY_TANK_EXPRESSION_LOVE:
    case BABY_TANK_EXPRESSION_PROUD:
        pool = happy;
        base_intensity = 72U;
        break;
    case BABY_TANK_EXPRESSION_EXCITED:
    case BABY_TANK_EXPRESSION_SURPRISED:
        pool = excited;
        base_intensity = 82U;
        break;
    case BABY_TANK_EXPRESSION_CURIOUS:
        pool = curious;
        base_intensity = 66U;
        break;
    case BABY_TANK_EXPRESSION_SLEEPY:
        pool = sleepy;
        base_intensity = 46U;
        break;
    case BABY_TANK_EXPRESSION_SAD:
    case BABY_TANK_EXPRESSION_CRYING:
        pool = sad;
        base_intensity = 58U;
        break;
    case BABY_TANK_EXPRESSION_WORRIED:
    case BABY_TANK_EXPRESSION_SICK:
        pool = worried;
        base_intensity = 61U;
        break;
    case BABY_TANK_EXPRESSION_ANNOYED:
    case BABY_TANK_EXPRESSION_COMPLAINING:
    case BABY_TANK_EXPRESSION_ANGRY:
    case BABY_TANK_EXPRESSION_FURIOUS:
        pool = annoyed;
        base_intensity = 74U;
        break;
    default:
        break;
    }
    uint8_t variant = (uint8_t)(esp_random() % 12U);
    if (variant == s_last_ambient_variant) variant = (uint8_t)((variant + 5U) % 12U);
    s_last_ambient_variant = variant;
    ambient_sequence_t sequence = {.count = variant % 3U == 0U ? 2U : 3U,
                                   .variant = variant};
    for (uint8_t step = 0U; step < sequence.count; ++step) {
        int intensity = (int)base_intensity +
            (int)((variant * 7U + step * 5U) % 13U) - 6;
        sequence.steps[step] = (baby_tank_face_request_t){
            .expression = pool[patterns[variant][step]],
            .duration_ms = 800U + (uint32_t)((variant + step * 3U) % 5U) * 170U,
            .intensity = clamp_intensity((uint8_t)intensity),
            .priority = BABY_TANK_FACE_PRIORITY_AMBIENT,
        };
    }
    return sequence;
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
        .expression = BABY_TANK_EXPRESSION_CALM,
        .duration_ms = UINT32_MAX,
        .intensity = 45U,
        .priority = BABY_TANK_FACE_PRIORITY_IDLE,
    };
    TickType_t active_start = xTaskGetTickCount();
    TickType_t frame_wake = active_start;
    TickType_t next_ambient = active_start + pdMS_TO_TICKS(20000U);
    TickType_t next_scene = 0U;
    TickType_t scene_started = 0U;
    bool scene_active = false;
    baby_tank_scene_t scene = BABY_TANK_SCENE_EXERCISE;
    baby_tank_scene_t last_scene_rendered = BABY_TANK_SCENE_COUNT;
    uint32_t last_scene_frame = UINT32_MAX;
    baby_tank_scene_t previous_scene = BABY_TANK_SCENE_COUNT;
    baby_tank_scene_t older_scene = BABY_TANK_SCENE_COUNT;
    ambient_sequence_t ambient_sequence = {0};
    uint8_t ambient_step = 0U;
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
        face_time_band_t time_band;
        TickType_t time_band_updated;
        read_idle_state(&forced_standby, &last_activity_tick,
                        &idle_expression, &idle_intensity,
                        &time_band, &time_band_updated);
        baby_tank_conversation_snapshot_t dialogue = {0};
        bool dialogue_awake =
            baby_tank_conversation_ready() &&
            baby_tank_conversation_get_snapshot(&dialogue) == ESP_OK &&
            dialogue.phase != BABY_TANK_CONVERSATION_IDLE &&
            dialogue.phase != BABY_TANK_CONVERSATION_CANCELLED &&
            dialogue.phase != BABY_TANK_CONVERSATION_ERROR &&
            dialogue.phase != BABY_TANK_CONVERSATION_DISCONNECTED;
        baby_tank_presence_state_t presence = presence_for_idle_time(
            now, last_activity_tick, forced_standby,
            time_band, time_band_updated);
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
        if (expired && active.priority == BABY_TANK_FACE_PRIORITY_AMBIENT &&
            ambient_step < ambient_sequence.count && !standby &&
            !dialogue_awake) {
            activate_request(&active, &active_start,
                             &ambient_sequence.steps[ambient_step++], now);
            elapsed_ms = 0U;
            expired = false;
        }
        if (expired) {
            ambient_sequence.count = 0U;
            ambient_step = 0U;
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
            ambient_sequence.count = 0U;
            ambient_step = 0U;
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

        bool fresh_daytime = time_band >= FACE_BAND_MORNING &&
            time_band <= FACE_BAND_EARLY_EVENING &&
            (uint32_t)((now - time_band_updated) * portTICK_PERIOD_MS) <
                FACE_TIME_BAND_FRESH_MS;
        uint32_t idle_ms = (uint32_t)((now - last_activity_tick) *
                                      portTICK_PERIOD_MS);
        uint32_t normal_ms = time_band == FACE_BAND_MIDDAY ||
                             time_band == FACE_BAND_EARLY_EVENING ?
                             180000U : 120000U;
        uint32_t bored_ms = normal_ms == 180000U ? 300000U : 180000U;
        baby_tank_scene_t preview_scene = BABY_TANK_SCENE_COUNT;
        TickType_t preview_started = 0U;
        bool preview_active = read_scene_preview(now, &preview_scene,
                                                  &preview_started);
        if (preview_active && (standby || dialogue_awake ||
            baby_tank_voice_runtime_remote_mode() ||
            active.priority >= BABY_TANK_FACE_PRIORITY_AI ||
            baby_tank_policy_is_locked())) {
            baby_tank_face_debug_stop_preview();
            preview_active = false;
        }
        bool scene_eligible = baby_tank_policy_time_known() && fresh_daytime &&
            idle_ms >= normal_ms + bored_ms &&
            presence == BABY_TANK_PRESENCE_BORED && !dialogue_awake &&
            !baby_tank_voice_runtime_remote_mode() &&
            active.priority == BABY_TANK_FACE_PRIORITY_IDLE &&
            !baby_tank_policy_is_locked() && !preview_active;
        if (scene_active && (!scene_eligible ||
            (uint32_t)((now - scene_started) * portTICK_PERIOD_MS) >=
                FACE_DAY_SCENE_DURATION_MS)) {
            bool completed = scene_eligible;
            if (!completed) {
                ESP_LOGI(TAG,
                    "pixel idle scene interrupted=%u idle_ms=%" PRIu32
                    " band=%u fresh=%u presence=%u dialogue=%u remote=%u"
                    " priority=%u locked=%u",
                    (unsigned)scene, idle_ms, (unsigned)time_band,
                    (unsigned)fresh_daytime, (unsigned)presence,
                    (unsigned)dialogue_awake,
                    (unsigned)baby_tank_voice_runtime_remote_mode(),
                    (unsigned)active.priority,
                    (unsigned)baby_tank_policy_is_locked());
            }
            scene_active = false;
            next_scene = now + pdMS_TO_TICKS(15U * 60U * 1000U);
            if (completed) {
                ESP_LOGI(TAG, "pixel idle scene completed=%u", (unsigned)scene);
                (void)baby_tank_state_event_enqueue(
                    BABY_TANK_STATE_EVENT_SCENE_COMPLETED);
            }
        }
        if (!scene_active && scene_eligible &&
            (next_scene == 0U || (int32_t)(now - next_scene) >= 0)) {
            static const baby_tank_scene_t morning[] = {
                BABY_TANK_SCENE_EXERCISE, BABY_TANK_SCENE_READING,
                BABY_TANK_SCENE_BLOCKS, BABY_TANK_SCENE_EXPLORE };
            static const baby_tank_scene_t midday[] = {
                BABY_TANK_SCENE_READING, BABY_TANK_SCENE_CLOUDS };
            static const baby_tank_scene_t afternoon[] = {
                BABY_TANK_SCENE_EXPLORE, BABY_TANK_SCENE_BLOCKS,
                BABY_TANK_SCENE_EXERCISE, BABY_TANK_SCENE_CLOUDS };
            static const baby_tank_scene_t evening[] = {
                BABY_TANK_SCENE_READING, BABY_TANK_SCENE_CLOUDS,
                BABY_TANK_SCENE_TIDY };
            const baby_tank_scene_t *pool = morning;
            size_t pool_count = sizeof(morning) / sizeof(morning[0]);
            if (time_band == FACE_BAND_MIDDAY) {
                pool = midday;
                pool_count = sizeof(midday) / sizeof(midday[0]);
            } else if (time_band == FACE_BAND_AFTERNOON) {
                pool = afternoon;
                pool_count = sizeof(afternoon) / sizeof(afternoon[0]);
            } else if (time_band == FACE_BAND_EARLY_EVENING) {
                pool = evening;
                pool_count = sizeof(evening) / sizeof(evening[0]);
            }
            size_t offset = esp_random() % pool_count;
            for (size_t trial = 0; trial < pool_count; ++trial) {
                baby_tank_scene_t candidate = pool[(offset + trial) % pool_count];
                if (candidate != previous_scene &&
                    (pool_count <= 2U || candidate != older_scene)) {
                    scene = candidate;
                    break;
                }
            }
            older_scene = previous_scene;
            previous_scene = scene;
            scene_active = true;
            scene_started = now;
            ESP_LOGI(TAG, "pixel idle scene started=%u", (unsigned)scene);
        }

        if (presence != BABY_TANK_PRESENCE_SLEEPING && !has_selected &&
            !scene_active && !preview_active &&
            active.priority == BABY_TANK_FACE_PRIORITY_IDLE &&
            (int32_t)(now - next_ambient) >= 0) {
            dialogue = (baby_tank_conversation_snapshot_t){0};
            if (baby_tank_conversation_get_snapshot(&dialogue) != ESP_OK ||
                dialogue.phase != BABY_TANK_CONVERSATION_IDLE) {
                next_ambient = now + pdMS_TO_TICKS(5000U);
            } else {
                baby_tank_expression_t sequence_mood =
                    presence == BABY_TANK_PRESENCE_BORED
                        ? BABY_TANK_EXPRESSION_THINKING : idle_expression;
                ambient_sequence = ambient_sequence_for_mood(sequence_mood);
                ambient_step = 1U;
                activate_request(&active, &active_start,
                                 &ambient_sequence.steps[0], now);
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
                if (preview_active) {
                    baby_tank_face_debug_stop_preview();
                    preview_active = false;
                }
                scene_active = false;
                next_scene = now + pdMS_TO_TICKS(15U * 60U * 1000U);
                render_result = baby_tank_display_render_drawing(&s_drawing);
            }
            xSemaphoreGive(s_drawing_mutex);
        }
        if (!show_drawing && preview_active &&
            active.priority == BABY_TANK_FACE_PRIORITY_IDLE) {
            uint32_t preview_elapsed_ms =
                (uint32_t)((now - preview_started) * portTICK_PERIOD_MS);
            uint32_t frame = preview_elapsed_ms / 900U;
            if (preview_scene != last_scene_rendered ||
                frame != last_scene_frame) {
                render_result = baby_tank_display_render_scene(preview_scene,
                    preview_elapsed_ms);
                if (render_result == ESP_OK) {
                    last_scene_rendered = preview_scene;
                    last_scene_frame = frame;
                }
            }
        } else if (!show_drawing && scene_active) {
            uint32_t scene_elapsed_ms =
                (uint32_t)((now - scene_started) * portTICK_PERIOD_MS);
            uint32_t frame = scene_elapsed_ms / 900U;
            if (scene != last_scene_rendered || frame != last_scene_frame) {
                render_result = baby_tank_display_render_scene(scene,
                    scene_elapsed_ms);
                if (render_result == ESP_OK) {
                    last_scene_rendered = scene;
                    last_scene_frame = frame;
                }
            }
        } else if (!show_drawing) {
            last_scene_rendered = BABY_TANK_SCENE_COUNT;
            last_scene_frame = UINT32_MAX;
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
    s_time_band = FACE_BAND_UNKNOWN;
    s_time_band_updated = 0U;
    s_preview_scene = BABY_TANK_SCENE_COUNT;
    s_preview_until = 0U;
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

esp_err_t baby_tank_face_set_time_band(const char *band)
{
    ESP_RETURN_ON_FALSE(band != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "time band is required");
    static const char *const names[] = {
        "morning", "midday", "afternoon", "early_evening",
        "evening", "late_night",
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strcmp(band, names[index]) == 0) {
            taskENTER_CRITICAL(&s_idle_lock);
            bool changed = s_time_band != (face_time_band_t)(index + 1U);
            s_time_band = (face_time_band_t)(index + 1U);
            s_time_band_updated = xTaskGetTickCount();
            taskEXIT_CRITICAL(&s_idle_lock);
            if (changed) ESP_LOGI(TAG, "verified time band=%s", band);
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t baby_tank_face_set_standby(bool standby)
{
    ESP_RETURN_ON_FALSE(s_face_task != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "face controller is not started");
    taskENTER_CRITICAL(&s_idle_lock);
    baby_tank_presence_state_t previous_state = s_presence_state;
    bool was_forced_standby = s_forced_standby;
    s_forced_standby = standby;
    s_preview_scene = BABY_TANK_SCENE_COUNT;
    s_preview_until = 0U;
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

esp_err_t baby_tank_face_debug_preview_scene(baby_tank_scene_t scene)
{
    ESP_RETURN_ON_FALSE(s_face_task != NULL &&
                            scene >= 0 && scene < BABY_TANK_SCENE_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid scene preview");
    ESP_RETURN_ON_FALSE(!baby_tank_policy_is_locked(), ESP_ERR_INVALID_STATE,
                        TAG, "scene preview blocked by policy");
    TickType_t now = xTaskGetTickCount();
    taskENTER_CRITICAL(&s_idle_lock);
    s_preview_scene = scene;
    s_preview_started = now;
    s_preview_until = now + pdMS_TO_TICKS(60000U);
    taskEXIT_CRITICAL(&s_idle_lock);
    return ESP_OK;
}

void baby_tank_face_debug_stop_preview(void)
{
    taskENTER_CRITICAL(&s_idle_lock);
    s_preview_scene = BABY_TANK_SCENE_COUNT;
    s_preview_until = 0U;
    taskEXIT_CRITICAL(&s_idle_lock);
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
        {"bird", BABY_TANK_THOUGHT_BIRD}, {"book", BABY_TANK_THOUGHT_BOOK},
        {"kite", BABY_TANK_THOUGHT_KITE},
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
                            drawing->duration_ms <= BABY_TANK_DRAWING_MAX_DURATION_MS,
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
