#include "voice_runtime.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "model_path.h"

#include "audio_io.h"
#include "cloud_client.h"
#include "chassis_bridge.h"
#include "camera_stream.h"
#include "runtime_policy.h"
#include "conversation.h"
#include "face_controller.h"
#include "network_runtime.h"
#include "interaction_memory.h"

#define VOICE_MAX_UTTERANCE_SECONDS 12U
#define VOICE_MAX_UTTERANCE_SAMPLES \
    (BABY_TANK_AUDIO_SAMPLE_RATE * VOICE_MAX_UTTERANCE_SECONDS)
#define VOICE_WAIT_FOR_SPEECH_MS 15000U
#define VOICE_MIN_UTTERANCE_MS 1000U
#define VOICE_END_SILENCE_MS 500U
#define VOICE_ENERGY_END_SILENCE_MS 650U
#define VOICE_ENERGY_MIN_SPEECH_MS 700U
#define VOICE_STATE_LOCK_MS 50U
#define VOICE_NETWORK_WAIT_MS 15000U
#define VOICE_FEED_TASK_STACK 4096U
#define VOICE_FETCH_TASK_STACK 5120U
#define VOICE_DIALOGUE_TASK_STACK 10240U
#define VOICE_FEED_TASK_PRIORITY 7U
#define VOICE_FETCH_TASK_PRIORITY 6U
#define VOICE_DIALOGUE_TASK_PRIORITY 5U
#define VOICE_ACTION_TASK_STACK 4096U
#define VOICE_ACTION_TASK_PRIORITY 6U
#define VOICE_PCM_STAGING_SAMPLES 512U
#define VOICE_PCM_PREBUFFER_SAMPLES 16000U
#define VOICE_PCM_PLAYBACK_CHUNK_SAMPLES 4096U
#define VOICE_PCM_RING_BYTES (128U * 1024U)
#define VOICE_PCM_PLAYER_STACK 5120U
#define VOICE_PCM_PLAYER_PRIORITY 6U
#define VOICE_TOUCH_REACTION_COOLDOWN_MS 1200U
#define VOICE_TOUCH_REACTION_RESET_MS 8000U
#define VOICE_TOUCH_REACTION_MAX_PENDING 2U
#define VOICE_TOUCH_RARE_ONE_IN 12U
#define VOICE_FACE_CUE_LIMIT 3U
#define VOICE_WAKE_CLIP_CHUNK_SAMPLES 256U
#define VOICE_REMOTE_MODE_LEASE_US 5000000LL
#define VOICE_RUNTIME_STOP_WAIT_MS 5000U
#define VOICE_REMOTE_RESUME_TASK_STACK 4096U
#define VOICE_PLAYBACK_TAIL_MS 250U
#define VOICE_POST_REPLY_MIC_GUARD_MS 900U
#define VOICE_POST_WAKE_MIC_GUARD_MS 300U

extern const uint8_t wake_wozai_start[] asm("_binary_wake_wozai_pcm_start");
extern const uint8_t wake_wozai_end[] asm("_binary_wake_wozai_pcm_end");
extern const uint8_t wake_lailao_start[] asm("_binary_wake_lailao_pcm_start");
extern const uint8_t wake_lailao_end[] asm("_binary_wake_lailao_pcm_end");
extern const uint8_t remote_mode_start[] asm("_binary_remote_mode_pcm_start");
extern const uint8_t remote_mode_end[] asm("_binary_remote_mode_pcm_end");
extern const uint8_t wake_please_move_start[] asm("_binary_wake_please_move_pcm_start");
extern const uint8_t wake_please_move_end[] asm("_binary_wake_please_move_pcm_end");
extern const uint8_t wake_coming_through_start[] asm("_binary_wake_coming_through_pcm_start");
extern const uint8_t wake_coming_through_end[] asm("_binary_wake_coming_through_pcm_end");
extern const uint8_t car_horn_start[] asm("_binary_car_horn_pcm_start");
extern const uint8_t car_horn_end[] asm("_binary_car_horn_pcm_end");
extern const uint8_t touch_huhu_start[] asm("_binary_baby_tank_touch_huhu_pcm_start");
extern const uint8_t touch_huhu_end[] asm("_binary_baby_tank_touch_huhu_pcm_end");
extern const uint8_t touch_dont_start[] asm("_binary_baby_tank_touch_dont_touch_pcm_start");
extern const uint8_t touch_dont_end[] asm("_binary_baby_tank_touch_dont_touch_pcm_end");
extern const uint8_t touch_angrier_start[] asm("_binary_baby_tank_touch_getting_angry_pcm_start");
extern const uint8_t touch_angrier_end[] asm("_binary_baby_tank_touch_getting_angry_pcm_end");
extern const uint8_t touch_stop_start[] asm("_binary_baby_tank_touch_stop_touching_pcm_start");
extern const uint8_t touch_stop_end[] asm("_binary_baby_tank_touch_stop_touching_pcm_end");
extern const uint8_t touch_left_ticklish_start[] asm("_binary_baby_tank_touch_left_ticklish_pcm_start");
extern const uint8_t touch_left_ticklish_end[] asm("_binary_baby_tank_touch_left_ticklish_pcm_end");
extern const uint8_t touch_left_again_start[] asm("_binary_baby_tank_touch_left_again_pcm_start");
extern const uint8_t touch_left_again_end[] asm("_binary_baby_tank_touch_left_again_pcm_end");
extern const uint8_t touch_left_found_me_start[] asm("_binary_baby_tank_touch_left_found_me_pcm_start");
extern const uint8_t touch_left_found_me_end[] asm("_binary_baby_tank_touch_left_found_me_pcm_end");
extern const uint8_t touch_left_itchy_start[] asm("_binary_baby_tank_touch_left_itchy_pcm_start");
extern const uint8_t touch_left_itchy_end[] asm("_binary_baby_tank_touch_left_itchy_pcm_end");
extern const uint8_t touch_left_comfy_start[] asm("_binary_baby_tank_touch_left_comfy_pcm_start");
extern const uint8_t touch_left_comfy_end[] asm("_binary_baby_tank_touch_left_comfy_pcm_end");
extern const uint8_t touch_left_like_you_start[] asm("_binary_baby_tank_touch_left_like_you_pcm_start");
extern const uint8_t touch_left_like_you_end[] asm("_binary_baby_tank_touch_left_like_you_pcm_end");
extern const uint8_t touch_right_who_start[] asm("_binary_baby_tank_touch_right_who_pcm_start");
extern const uint8_t touch_right_who_end[] asm("_binary_baby_tank_touch_right_who_pcm_end");
extern const uint8_t touch_right_not_startled_start[] asm("_binary_baby_tank_touch_right_not_startled_pcm_start");
extern const uint8_t touch_right_not_startled_end[] asm("_binary_baby_tank_touch_right_not_startled_pcm_end");
extern const uint8_t touch_right_too_start[] asm("_binary_baby_tank_touch_right_too_pcm_start");
extern const uint8_t touch_right_too_end[] asm("_binary_baby_tank_touch_right_too_pcm_end");
extern const uint8_t touch_right_again_start[] asm("_binary_baby_tank_touch_right_again_pcm_start");
extern const uint8_t touch_right_again_end[] asm("_binary_baby_tank_touch_right_again_pcm_end");
extern const uint8_t touch_right_one_more_start[] asm("_binary_baby_tank_touch_right_one_more_pcm_start");
extern const uint8_t touch_right_one_more_end[] asm("_binary_baby_tank_touch_right_one_more_pcm_end");
extern const uint8_t touch_right_watching_start[] asm("_binary_baby_tank_touch_right_watching_pcm_start");
extern const uint8_t touch_right_watching_end[] asm("_binary_baby_tank_touch_right_watching_pcm_end");
extern const uint8_t touch_rare_sneeze_start[] asm("_binary_baby_tank_touch_rare_sneeze_pcm_start");
extern const uint8_t touch_rare_sneeze_end[] asm("_binary_baby_tank_touch_rare_sneeze_pcm_end");
extern const uint8_t touch_rare_catch_me_start[] asm("_binary_baby_tank_touch_rare_catch_me_pcm_start");
extern const uint8_t touch_rare_catch_me_end[] asm("_binary_baby_tank_touch_rare_catch_me_pcm_end");
extern const uint8_t touch_rare_easter_egg_start[] asm("_binary_baby_tank_touch_rare_easter_egg_pcm_start");
extern const uint8_t touch_rare_easter_egg_end[] asm("_binary_baby_tank_touch_rare_easter_egg_pcm_end");
extern const uint8_t touch_rare_transform_start[] asm("_binary_baby_tank_touch_rare_transform_pcm_start");
extern const uint8_t touch_rare_transform_end[] asm("_binary_baby_tank_touch_rare_transform_pcm_end");
extern const uint8_t game_win_start[] asm("_binary_game_win_pcm_start");
extern const uint8_t game_win_end[] asm("_binary_game_win_pcm_end");
extern const uint8_t game_lose_start[] asm("_binary_game_lose_pcm_start");
extern const uint8_t game_lose_end[] asm("_binary_game_lose_pcm_end");
extern const uint8_t game_draw_start[] asm("_binary_game_draw_pcm_start");
extern const uint8_t game_draw_end[] asm("_binary_game_draw_pcm_end");

typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_LISTENING,
    VOICE_STATE_PROCESSING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_ACTING,
} voice_state_t;

typedef enum {
    VOICE_REQUEST_RECORDED_SPEECH = 0,
    VOICE_REQUEST_TOUCH_REACTION,
    VOICE_REQUEST_CLOUD_TEXT,
} voice_request_kind_t;

typedef struct {
    size_t sample_offset;
    char emotion[24];
    uint8_t intensity;
} face_cue_t;

typedef struct {
    voice_request_kind_t kind;
    uint32_t turn_id;
    baby_tank_touch_reaction_t touch_side;
    uint8_t touch_stage;
    char text_job_id[40];
} voice_request_t;

typedef struct {
    bool has_tail;
    uint8_t tail;
    int16_t *prebuffer;
    size_t prebuffered_samples;
    atomic_bool playback_started;
    atomic_int_fast64_t first_playback_us;
    uint8_t *stream_storage;
    StaticStreamBuffer_t stream_control;
    StreamBufferHandle_t stream;
    TaskHandle_t player_task;
    TaskHandle_t dialogue_task;
    atomic_bool producer_done;
    atomic_bool player_abort;
    atomic_bool player_finished;
    esp_err_t player_result;
    size_t ring_peak_bytes;
    uint32_t ring_empty_waits;
    UBaseType_t player_stack_spare_bytes;
    size_t received_bytes;
    int64_t codec_write_us;
    int64_t last_callback_end_us;
    int64_t max_source_gap_us;
    int64_t min_playback_headroom_ms;
    uint32_t source_gaps_over_100ms;
    size_t uploaded_samples;
    atomic_size_t written_samples;
    face_cue_t face_cues[VOICE_FACE_CUE_LIMIT];
    atomic_uchar face_cue_count;
    uint8_t next_face_cue;
    baby_tank_cloud_drive_action_t *action;
    atomic_bool action_started;
    atomic_bool action_done;
    esp_err_t action_result;
    uint32_t action_epoch;
} pcm_sink_t;

static const char *TAG = "baby_tank_voice";
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
static SemaphoreHandle_t s_state_mutex;
static QueueHandle_t s_utterance_queue;
static TaskHandle_t s_feed_task;
static TaskHandle_t s_fetch_task;
static TaskHandle_t s_dialogue_task;
static int16_t *s_recording;
static size_t s_recorded_samples;
static uint32_t s_waited_ms;
static uint32_t s_silence_ms;
static uint32_t s_energy_quiet_ms;
static uint32_t s_peak_frame_level;
static uint32_t s_turn_id;
static bool s_speech_started;
static bool s_cancel_requested;
static bool s_pending_listen;
static bool s_session_active;
/* Absolute deadline prevents false VAD/empty ASR turns from repeatedly
 * restarting the 15-second listening window. */
static int64_t s_listen_deadline_us;
static int64_t s_ignore_capture_until_us;
static TickType_t s_last_touch_reaction;
static uint8_t s_touch_stage;
static uint8_t s_last_touch_choice[2] = {UINT8_MAX, UINT8_MAX};
static uint8_t s_last_touch_rare = UINT8_MAX;
static bool s_touch_reaction_active;
static voice_state_t s_state = VOICE_STATE_IDLE;
/* Covers the whole model-owned drive/drawing interval. Unlike
 * s_explore_active, this remains true until the dialogue task has consumed
 * the action result and is ready to reopen the microphone. */
static atomic_bool s_action_active;
static atomic_bool s_drawing_active;
static atomic_bool s_drawing_cancel;
static atomic_bool s_explore_active;
static atomic_bool s_explore_cancel;
static atomic_bool s_wake_ack_busy;
static atomic_bool s_remote_mode;
static atomic_bool s_runtime_stopping;
static atomic_bool s_dialogue_busy;
static atomic_bool s_remote_resume_pending;
static atomic_int_fast64_t s_remote_mode_deadline_us;
static atomic_int_fast64_t s_capture_finished_us;
static SemaphoreHandle_t s_mode_mutex;
static esp_timer_handle_t s_remote_lease_timer;

void baby_tank_voice_runtime_log_memory(void)
{
    multi_heap_info_t heap = {0};
    heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "memory internal_free=%zu largest=%zu min_free=%zu blocks=%zu psram_free=%zu",
             heap.total_free_bytes, heap.largest_free_block,
             heap.minimum_free_bytes, heap.free_blocks,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (s_feed_task != NULL && s_fetch_task != NULL &&
        s_dialogue_task != NULL) {
        ESP_LOGI(TAG, "stack spare bytes feed=%u/%u fetch=%u/%u dialogue=%u/%u",
                 (unsigned)uxTaskGetStackHighWaterMark(s_feed_task),
                 VOICE_FEED_TASK_STACK,
                 (unsigned)uxTaskGetStackHighWaterMark(s_fetch_task),
                 VOICE_FETCH_TASK_STACK,
                 (unsigned)uxTaskGetStackHighWaterMark(s_dialogue_task),
                 VOICE_DIALOGUE_TASK_STACK);
    }
}

static uint32_t frame_duration_ms(int data_size)
{
    size_t samples = data_size > 0 ? (size_t)data_size / sizeof(int16_t) : 0U;
    uint32_t duration =
        (uint32_t)(samples * 1000U / BABY_TANK_AUDIO_SAMPLE_RATE);
    return duration > 0U ? duration : 1U;
}

static void reset_capture_locked(void)
{
    s_recorded_samples = 0U;
    s_waited_ms = 0U;
    s_silence_ms = 0U;
    s_energy_quiet_ms = 0U;
    s_peak_frame_level = 0U;
    s_speech_started = false;
    s_ignore_capture_until_us = 0;
    atomic_store(&s_capture_finished_us, 0);
}

static bool take_state_lock(void)
{
    return xSemaphoreTake(s_state_mutex,
                          pdMS_TO_TICKS(VOICE_STATE_LOCK_MS)) == pdTRUE;
}

static bool cancellation_requested(void)
{
    if (!take_state_lock()) return true;
    bool requested = s_cancel_requested;
    xSemaphoreGive(s_state_mutex);
    return requested;
}

static void set_phase(baby_tank_conversation_phase_t phase)
{
    esp_err_t result = baby_tank_conversation_set_phase(phase);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "conversation phase %s failed: %s",
                 baby_tank_conversation_phase_name(phase),
                 esp_err_to_name(result));
    }
}

static void configure_wakenet(bool enabled)
{
    if (s_afe == NULL || s_afe_data == NULL) return;
    int result = enabled ? s_afe->enable_wakenet(s_afe_data)
                         : s_afe->disable_wakenet(s_afe_data);
    if (result < 0) {
        ESP_LOGW(TAG, "unable to %s WakeNet", enabled ? "enable" : "disable");
    }
}

static void enter_action_state(const char *kind)
{
    atomic_store(&s_action_active, true);
    if (take_state_lock()) {
        s_state = VOICE_STATE_ACTING;
        xSemaphoreGive(s_state_mutex);
    }
    configure_wakenet(false);
    ESP_LOGI(TAG, "AI %s owns voice state; listening deferred", kind);
}

esp_err_t baby_tank_voice_runtime_play_phrase(uint8_t phrase)
{
    if (phrase > 8U) return ESP_ERR_INVALID_ARG;
    const uint8_t *starts[] = {
        wake_wozai_start, wake_lailao_start, remote_mode_start,
        wake_please_move_start, wake_coming_through_start, car_horn_start,
        game_win_start, game_lose_start, game_draw_start,
    };
    const uint8_t *ends[] = {
        wake_wozai_end, wake_lailao_end, remote_mode_end,
        wake_please_move_end, wake_coming_through_end, car_horn_end,
        game_win_end, game_lose_end, game_draw_end,
    };
    const char *labels[] = {
        "我在呢", "来啦", "已进入遥控模式", "让一让",
        "我想过一下", "汽车喇叭", "我赢了", "我输了", "平局啦",
    };
    const uint8_t *start = starts[phrase];
    const uint8_t *end = ends[phrase];
    size_t bytes = (size_t)(end - start);
    if (bytes < 3200U || bytes > 128000U || (bytes & 1U) != 0U) {
        ESP_LOGW(TAG, "invalid embedded wake clip size=%zu", bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t result = baby_tank_audio_io_begin_playback();
    if (result != ESP_OK) return result;
    int16_t chunk[VOICE_WAKE_CLIP_CHUNK_SAMPLES];
    for (size_t offset = 0U; offset < bytes; offset += sizeof(chunk)) {
        size_t count = bytes - offset;
        if (count > sizeof(chunk)) count = sizeof(chunk);
        memcpy(chunk, start + offset, count);
        result = baby_tank_audio_io_write(chunk, count / sizeof(int16_t));
        if (result != ESP_OK) break;
    }
    /* esp_codec_dev_write may return with samples still in the I2S DMA
     * queue. Keep microphone feed/fetch paused until that tail is silent. */
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(VOICE_PLAYBACK_TAIL_MS));
    }
    baby_tank_audio_io_end_playback();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "wake clip playback failed: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(TAG, "embedded clip played: %s %zu ms",
                 labels[phrase], bytes / 32U);
    }
    return result;
}

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const char *label;
    const char *emotion;
    baby_tank_expression_t expression;
    uint8_t intensity;
} touch_clip_t;

#define TOUCH_CLIP(start_name, end_name, clip_label, emotion_name, face, level) \
    { start_name, end_name, clip_label, emotion_name, face, level }

static const touch_clip_t TOUCH_LEFT_CLIPS[] = {
    TOUCH_CLIP(touch_left_ticklish_start, touch_left_ticklish_end,
               "left_ticklish", "laughing", BABY_TANK_EXPRESSION_LAUGHING, 84U),
    TOUCH_CLIP(touch_left_again_start, touch_left_again_end,
               "left_again", "happy", BABY_TANK_EXPRESSION_HAPPY, 80U),
    TOUCH_CLIP(touch_left_found_me_start, touch_left_found_me_end,
               "left_found_me", "surprised", BABY_TANK_EXPRESSION_SURPRISED, 82U),
    TOUCH_CLIP(touch_left_itchy_start, touch_left_itchy_end,
               "left_itchy", "shy", BABY_TANK_EXPRESSION_SHY, 76U),
    TOUCH_CLIP(touch_left_comfy_start, touch_left_comfy_end,
               "left_comfy", "love", BABY_TANK_EXPRESSION_LOVE, 78U),
    TOUCH_CLIP(touch_left_like_you_start, touch_left_like_you_end,
               "left_like_you", "love", BABY_TANK_EXPRESSION_LOVE, 86U),
};

static const touch_clip_t TOUCH_RIGHT_CLIPS[] = {
    TOUCH_CLIP(touch_right_who_start, touch_right_who_end,
               "right_who", "curious", BABY_TANK_EXPRESSION_CURIOUS, 80U),
    TOUCH_CLIP(touch_right_not_startled_start, touch_right_not_startled_end,
               "right_not_startled", "proud", BABY_TANK_EXPRESSION_PROUD, 84U),
    TOUCH_CLIP(touch_right_too_start, touch_right_too_end,
               "right_too", "surprised", BABY_TANK_EXPRESSION_SURPRISED, 78U),
    TOUCH_CLIP(touch_right_again_start, touch_right_again_end,
               "right_again", "annoyed", BABY_TANK_EXPRESSION_ANNOYED, 72U),
    TOUCH_CLIP(touch_right_one_more_start, touch_right_one_more_end,
               "right_one_more", "shy", BABY_TANK_EXPRESSION_SHY, 76U),
    TOUCH_CLIP(touch_right_watching_start, touch_right_watching_end,
               "right_watching", "proud", BABY_TANK_EXPRESSION_PROUD, 82U),
};

static const touch_clip_t TOUCH_RARE_CLIPS[] = {
    TOUCH_CLIP(touch_rare_sneeze_start, touch_rare_sneeze_end,
               "rare_sneeze", "dizzy", BABY_TANK_EXPRESSION_DIZZY, 92U),
    TOUCH_CLIP(touch_rare_catch_me_start, touch_rare_catch_me_end,
               "rare_catch_me", "laughing", BABY_TANK_EXPRESSION_LAUGHING, 94U),
    TOUCH_CLIP(touch_rare_easter_egg_start, touch_rare_easter_egg_end,
               "rare_easter_egg", "excited", BABY_TANK_EXPRESSION_EXCITED, 96U),
    TOUCH_CLIP(touch_rare_transform_start, touch_rare_transform_end,
               "rare_transform", "bubble", BABY_TANK_EXPRESSION_BUBBLE, 94U),
};

static const touch_clip_t TOUCH_ESCALATION_CLIPS[] = {
    TOUCH_CLIP(touch_dont_start, touch_dont_end,
               "slow_down", "surprised", BABY_TANK_EXPRESSION_SURPRISED, 82U),
    TOUCH_CLIP(touch_angrier_start, touch_angrier_end,
               "getting_angry", "annoyed", BABY_TANK_EXPRESSION_ANNOYED, 90U),
    TOUCH_CLIP(touch_stop_start, touch_stop_end,
               "stop_touching", "angry", BABY_TANK_EXPRESSION_ANGRY, 98U),
};

static uint8_t choose_non_repeating(size_t count, uint8_t *last)
{
    if (count <= 1U) return 0U;
    uint8_t choice = (uint8_t)(esp_random() % count);
    if (choice == *last) {
        choice = (uint8_t)((choice + 1U + esp_random() % (count - 1U)) % count);
    }
    *last = choice;
    return choice;
}

static const touch_clip_t *choose_touch_clip(
    baby_tank_touch_reaction_t side, uint8_t stage)
{
    if (stage >= 2U) {
        size_t escalation = stage - 2U;
        if (escalation >= sizeof(TOUCH_ESCALATION_CLIPS) /
                              sizeof(TOUCH_ESCALATION_CLIPS[0])) {
            escalation = sizeof(TOUCH_ESCALATION_CLIPS) /
                         sizeof(TOUCH_ESCALATION_CLIPS[0]) - 1U;
        }
        return &TOUCH_ESCALATION_CLIPS[escalation];
    }
    if (esp_random() % VOICE_TOUCH_RARE_ONE_IN == 0U) {
        uint8_t choice = choose_non_repeating(
            sizeof(TOUCH_RARE_CLIPS) / sizeof(TOUCH_RARE_CLIPS[0]),
            &s_last_touch_rare);
        return &TOUCH_RARE_CLIPS[choice];
    }
    const touch_clip_t *pool = side == BABY_TANK_TOUCH_REACTION_LEFT
                                   ? TOUCH_LEFT_CLIPS : TOUCH_RIGHT_CLIPS;
    size_t count = side == BABY_TANK_TOUCH_REACTION_LEFT
                       ? sizeof(TOUCH_LEFT_CLIPS) / sizeof(TOUCH_LEFT_CLIPS[0])
                       : sizeof(TOUCH_RIGHT_CLIPS) / sizeof(TOUCH_RIGHT_CLIPS[0]);
    uint8_t side_index = side == BABY_TANK_TOUCH_REACTION_LEFT ? 0U : 1U;
    uint8_t choice = choose_non_repeating(count,
                                           &s_last_touch_choice[side_index]);
    return &pool[choice];
}

static esp_err_t play_touch_reaction(baby_tank_touch_reaction_t side,
                                     uint8_t stage)
{
    const touch_clip_t *clip = choose_touch_clip(side, stage);
    size_t bytes = (size_t)(clip->end - clip->start);
    if (bytes < 3200U || bytes > 128000U || (bytes & 1U) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    set_phase(BABY_TANK_CONVERSATION_AI_SPEAKING);
    ESP_RETURN_ON_ERROR(baby_tank_conversation_set_ai_emotion(
                            clip->emotion, clip->intensity),
                        TAG, "touch emotion failed");
    const baby_tank_face_request_t face = {
        .expression = clip->expression,
        .duration_ms = (uint32_t)(bytes / 32U) + 1000U,
        .intensity = clip->intensity,
        .priority = BABY_TANK_FACE_PRIORITY_INTERACTION,
    };
    ESP_RETURN_ON_ERROR(baby_tank_face_submit(&face), TAG,
                        "touch face failed");
    ESP_RETURN_ON_ERROR(baby_tank_audio_io_begin_playback(), TAG,
                        "touch playback failed");
    esp_err_t result = ESP_OK;
    int16_t chunk[VOICE_WAKE_CLIP_CHUNK_SAMPLES];
    for (size_t offset = 0U; offset < bytes; offset += sizeof(chunk)) {
        size_t count = bytes - offset;
        if (count > sizeof(chunk)) count = sizeof(chunk);
        memcpy(chunk, clip->start + offset, count);
        result = baby_tank_conversation_observe_tts_pcm16(
            chunk, count / sizeof(int16_t));
        if (result == ESP_OK) {
            result = baby_tank_audio_io_write(chunk,
                                              count / sizeof(int16_t));
        }
        if (result != ESP_OK) break;
        if (cancellation_requested()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
    }
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(VOICE_PLAYBACK_TAIL_MS));
    }
    baby_tank_audio_io_end_playback();
    ESP_LOGI(TAG,
             "local %s-touch reaction streak=%u clip=%s emotion=%s audio_ms=%zu result=%s",
             side == BABY_TANK_TOUCH_REACTION_LEFT ? "left" : "right",
             (unsigned)(stage + 1U), clip->label, clip->emotion,
             bytes / 32U, esp_err_to_name(result));
    return result;
}

static void play_wake_ack(uint32_t turn_id)
{
    (void)baby_tank_voice_runtime_play_phrase(
        (turn_id & 1U) != 0U ? 0U : 1U);
}

static void complete_turn(void)
{
    bool listen_again = false;
    /* Release action ownership only at the common completion boundary. A
     * deferred wake/touch can therefore never open the microphone while a
     * motor pulse or drawing hold is still running. */
    atomic_store(&s_action_active, false);
    if (take_state_lock()) {
        listen_again = (s_pending_listen || s_session_active) &&
                       !s_cancel_requested;
        s_pending_listen = false;
        s_cancel_requested = false;
        baby_tank_cloud_reset_cancel();
        reset_capture_locked();
        s_state = listen_again ? VOICE_STATE_LISTENING : VOICE_STATE_IDLE;
        if (listen_again) {
            s_ignore_capture_until_us = esp_timer_get_time() +
                (int64_t)VOICE_POST_REPLY_MIC_GUARD_MS * 1000LL;
        }
        if (listen_again) ++s_turn_id;
        xSemaphoreGive(s_state_mutex);
    }
    s_afe->reset_buffer(s_afe_data);
    configure_wakenet(!listen_again && !atomic_load(&s_remote_mode) &&
                      !baby_tank_policy_is_locked());
    ESP_LOGI(TAG, "conversation turn complete next=%s session_active=%d",
             listen_again ? "listening" : "standby", listen_again ? 1 : 0);
    if (listen_again) {
        (void)baby_tank_face_set_standby(false);
        baby_tank_conversation_snapshot_t snapshot;
        if (baby_tank_conversation_get_snapshot(&snapshot) == ESP_OK &&
            snapshot.phase == BABY_TANK_CONVERSATION_IDLE) {
            esp_err_t resumed = baby_tank_conversation_resume_listening();
            if (resumed != ESP_OK) set_phase(BABY_TANK_CONVERSATION_LISTENING);
        } else {
            set_phase(BABY_TANK_CONVERSATION_LISTENING);
        }
    } else {
        (void)baby_tank_face_note_activity();
        baby_tank_conversation_snapshot_t snapshot;
        if (baby_tank_conversation_get_snapshot(&snapshot) != ESP_OK ||
            snapshot.phase != BABY_TANK_CONVERSATION_IDLE) {
            set_phase(BABY_TANK_CONVERSATION_IDLE);
        }
        /* Keep the authenticated voice transport warm in standby. Closing it
         * here made every new wake pay the full TLS handshake again. Remote
         * mode, deep-night lock, network failure and runtime stop still close
         * it explicitly. */
    }
}

esp_err_t baby_tank_voice_runtime_request_listen(void)
{
    ESP_RETURN_ON_FALSE(!baby_tank_policy_is_locked(), ESP_ERR_INVALID_STATE,
                        TAG, "voice wake blocked by deep-night policy");
    ESP_RETURN_ON_FALSE(!atomic_load(&s_remote_mode), ESP_ERR_INVALID_STATE,
                        TAG, "AI voice is paused in remote mode");
    if (atomic_load(&s_action_active)) {
        if (atomic_load(&s_drawing_active)) {
            /* A wake-word edge is not an explicit request to dismiss art.
             * Keep the canvas and microphone ownership through its hold;
             * the ordinary session resumes listening afterwards. */
            ESP_LOGI(TAG, "wake ignored while AI drawing is displayed");
            return ESP_OK;
        }
        if (take_state_lock()) {
            s_pending_listen = true;
            s_session_active = true;
            xSemaphoreGive(s_state_mutex);
        }
        ESP_LOGI(TAG, "wake deferred until AI action completes");
        return ESP_OK;
    }
    if (atomic_load(&s_explore_active)) {
        atomic_store(&s_explore_cancel, true);
        baby_tank_chassis_bridge_stop();
        ESP_LOGW(TAG, "top touch/wake stopped autonomous group");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_state_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "voice runtime is not initialized");
    ESP_RETURN_ON_FALSE(take_state_lock(), ESP_ERR_TIMEOUT, TAG,
                        "voice state is busy");
    voice_state_t previous = s_state;
    if (previous == VOICE_STATE_IDLE) {
        s_state = VOICE_STATE_LISTENING;
        s_session_active = true;
        s_cancel_requested = false;
        s_pending_listen = false;
        ++s_turn_id;
        reset_capture_locked();
    }
    xSemaphoreGive(s_state_mutex);
    if (previous != VOICE_STATE_IDLE) {
        /* The capacitive pad still produces occasional false edges. A
         * second touch must not cancel an ASR/TLS upload or TTS playback. */
        ESP_LOGI(TAG, "touch/wake ignored while dialogue state=%d", previous);
        return ESP_OK;
    }
    configure_wakenet(false);
    (void)baby_tank_face_set_standby(false);
    baby_tank_cloud_reset_cancel();
    /* Never block the AFE consumer while an idle App-text request exits: doing
     * so fills the microphone ring buffer and can discard the beginning of a
     * child's question. The waiter is bounded to 250 ms server-side and owns
     * its handle until it observes this cancellation flag. */
    baby_tank_cloud_cancel_text_wait();
    /* Start TLS while the local wake phrase plays. The session remains open
     * across turns and closes when the 15-second conversation expires. */
    if (baby_tank_network_cloud_is_ready()) {
        (void)baby_tank_cloud_voice_session_prepare();
    }
    s_afe->reset_buffer(s_afe_data);
    set_phase(BABY_TANK_CONVERSATION_LISTENING);
    const baby_tank_face_request_t wake_face = {
        .expression = BABY_TANK_EXPRESSION_SURPRISED,
        .duration_ms = 2300U,
        .intensity = 92U,
        .priority = BABY_TANK_FACE_PRIORITY_INTERACTION,
    };
    baby_tank_face_submit(&wake_face);
    atomic_store(&s_wake_ack_busy, true);
    play_wake_ack(s_turn_id);
    s_afe->reset_buffer(s_afe_data);
    if (take_state_lock()) {
        s_ignore_capture_until_us = esp_timer_get_time() +
            (int64_t)VOICE_POST_WAKE_MIC_GUARD_MS * 1000LL;
        s_listen_deadline_us = s_ignore_capture_until_us +
            (int64_t)VOICE_WAIT_FOR_SPEECH_MS * 1000LL;
        xSemaphoreGive(s_state_mutex);
    }
    atomic_store(&s_wake_ack_busy, false);
    ESP_LOGI(TAG, "new conversation wake turn=%lu listen_window_ms=%u",
             (unsigned long)s_turn_id, (unsigned)VOICE_WAIT_FOR_SPEECH_MS);
    return ESP_OK;
}

esp_err_t baby_tank_voice_runtime_cancel_conversation(void)
{
    ESP_RETURN_ON_FALSE(!atomic_load(&s_remote_mode), ESP_ERR_INVALID_STATE,
                        TAG, "voice conversation is unavailable in remote mode");
    ESP_RETURN_ON_FALSE(s_state_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "voice runtime is not initialized");

    /* This is an explicit user action, unlike a noisy second touch edge, so it
     * is allowed to stop every foreground phase including ASR/TLS/TTS. */
    atomic_store(&s_drawing_cancel, true);
    baby_tank_face_cancel_drawing();
    atomic_store(&s_explore_cancel, true);
    (void)baby_tank_chassis_bridge_stop();
    ESP_RETURN_ON_FALSE(take_state_lock(), ESP_ERR_TIMEOUT, TAG,
                        "voice state is busy");
    s_session_active = false;
    s_pending_listen = false;
    s_cancel_requested = true;
    s_state = VOICE_STATE_IDLE;
    reset_capture_locked();
    xSemaphoreGive(s_state_mutex);

    baby_tank_cloud_cancel_current();
    baby_tank_cloud_voice_session_close();
    baby_tank_audio_io_end_playback();
    if (s_afe != NULL && s_afe_data != NULL) s_afe->reset_buffer(s_afe_data);
    set_phase(BABY_TANK_CONVERSATION_IDLE);
    configure_wakenet(!baby_tank_policy_is_locked());
    (void)baby_tank_face_set_standby(false);
    (void)baby_tank_face_note_activity();
    ESP_LOGI(TAG, "conversation cancelled by App; WakeNet restored");
    return ESP_OK;
}

bool baby_tank_voice_runtime_can_accept_text(void)
{
    if (baby_tank_policy_is_locked() || atomic_load(&s_remote_mode) ||
        atomic_load(&s_dialogue_busy) || atomic_load(&s_action_active) ||
        s_state_mutex == NULL || s_utterance_queue == NULL ||
        !take_state_lock()) return false;
    bool available = s_state == VOICE_STATE_IDLE &&
        uxQueueMessagesWaiting(s_utterance_queue) == 0U;
    xSemaphoreGive(s_state_mutex);
    return available;
}

esp_err_t baby_tank_voice_runtime_request_cloud_text(const char *job_id)
{
    ESP_RETURN_ON_FALSE(job_id != NULL && strlen(job_id) == 36U &&
                        baby_tank_voice_runtime_can_accept_text(),
                        ESP_ERR_INVALID_STATE, TAG,
                        "typed turn cannot interrupt active interaction");
    ESP_RETURN_ON_FALSE(take_state_lock(), ESP_ERR_TIMEOUT, TAG,
                        "typed turn voice state busy");
    if (s_state != VOICE_STATE_IDLE || baby_tank_policy_is_locked() ||
        atomic_load(&s_remote_mode)) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    voice_request_t request = {
        .kind = VOICE_REQUEST_CLOUD_TEXT,
        .turn_id = ++s_turn_id,
    };
    snprintf(request.text_job_id, sizeof(request.text_job_id), "%s", job_id);
    if (xQueueSend(s_utterance_queue, &request, 0) != pdTRUE) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_TIMEOUT;
    }
    s_state = VOICE_STATE_PROCESSING;
    s_session_active = false;
    s_pending_listen = false;
    s_cancel_requested = false;
    atomic_store(&s_action_active, true);
    xSemaphoreGive(s_state_mutex);
    configure_wakenet(false);
    s_afe->reset_buffer(s_afe_data);
    baby_tank_cloud_reset_cancel();
    (void)baby_tank_face_set_standby(false);
    (void)baby_tank_face_note_activity();
    set_phase(BABY_TANK_CONVERSATION_THINKING);
    ESP_LOGI(TAG, "cloud text turn accepted turn=%lu",
             (unsigned long)request.turn_id);
    return ESP_OK;
}

esp_err_t baby_tank_voice_runtime_toggle_listen(void)
{
    ESP_RETURN_ON_FALSE(!baby_tank_policy_is_locked(), ESP_ERR_INVALID_STATE,
                        TAG, "top touch blocked by deep-night policy");
    ESP_RETURN_ON_FALSE(!atomic_load(&s_remote_mode), ESP_ERR_INVALID_STATE,
                        TAG, "top touch is paused in remote mode");
    if (atomic_load(&s_action_active)) {
        /* Touch cancels a drawing hold or stops motion, then starts listening
         * only after the action task has released voice ownership. */
        if (atomic_load(&s_drawing_active)) {
            atomic_store(&s_drawing_cancel, true);
        }
        if (atomic_load(&s_explore_active)) {
            atomic_store(&s_explore_cancel, true);
            (void)baby_tank_chassis_bridge_stop();
        }
        if (take_state_lock()) {
            s_pending_listen = true;
            s_session_active = true;
            xSemaphoreGive(s_state_mutex);
        }
        ESP_LOGI(TAG, "top touch deferred listening until AI action completes");
        return ESP_OK;
    }
    if (atomic_load(&s_explore_active)) {
        atomic_store(&s_explore_cancel, true);
        baby_tank_chassis_bridge_stop();
        ESP_LOGI(TAG, "top touch stopped autonomous group");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_state_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "voice runtime is not initialized");
    ESP_RETURN_ON_FALSE(take_state_lock(), ESP_ERR_TIMEOUT, TAG,
                        "voice state is busy");
    if (s_state == VOICE_STATE_IDLE) {
        xSemaphoreGive(s_state_mutex);
        return baby_tank_voice_runtime_request_listen();
    }

    voice_state_t current = s_state;
    xSemaphoreGive(s_state_mutex);
    /* A second capacitive edge while the child speaks can be genuine or
     * coupled noise. Neither should discard a captured utterance. The
     * session closes on the spoken rest command or its idle timeout. */
    ESP_LOGI(TAG, "top touch ignored during conversation state=%d", current);
    return ESP_OK;
}

bool baby_tank_voice_runtime_remote_mode(void)
{
    return atomic_load(&s_remote_mode);
}

void baby_tank_voice_runtime_enter_night_lock(void)
{
    atomic_store(&s_drawing_cancel, true);
    baby_tank_face_cancel_drawing();
    atomic_store(&s_explore_cancel, true);
    (void)baby_tank_chassis_bridge_stop();
    if (s_state_mutex != NULL && take_state_lock()) {
        s_session_active = false;
        s_pending_listen = false;
        s_cancel_requested = true;
        s_state = VOICE_STATE_IDLE;
        reset_capture_locked();
        xSemaphoreGive(s_state_mutex);
    }
    baby_tank_cloud_cancel_current();
    baby_tank_cloud_voice_session_close();
    configure_wakenet(false);
    ESP_LOGW(TAG, "active child dialogue cancelled for deep-night lock");
}

void baby_tank_voice_runtime_exit_night_lock(void)
{
    if (!atomic_load(&s_remote_mode)) configure_wakenet(true);
    ESP_LOGI(TAG, "deep-night lock ended; waiting for an explicit wake");
}

esp_err_t baby_tank_voice_runtime_request_sleep(void)
{
    /* Remote/video mode owns the camera and has torn down the voice runtime.
     * Restore normal voice ownership first so the child can wake the robot
     * again without reopening the App. */
    if (atomic_load(&s_remote_mode)) {
        ESP_RETURN_ON_ERROR(baby_tank_voice_runtime_set_remote_mode(false),
                            TAG, "leave remote mode before sleep failed");
    }

    atomic_store(&s_drawing_cancel, true);
    baby_tank_face_cancel_drawing();
    atomic_store(&s_explore_cancel, true);
    (void)baby_tank_chassis_bridge_stop();
    if (s_state_mutex != NULL && take_state_lock()) {
        s_session_active = false;
        s_pending_listen = false;
        s_cancel_requested = true;
        s_state = VOICE_STATE_IDLE;
        reset_capture_locked();
        xSemaphoreGive(s_state_mutex);
    }
    baby_tank_cloud_cancel_current();
    baby_tank_cloud_voice_session_close();
    baby_tank_audio_io_end_playback();
    set_phase(BABY_TANK_CONVERSATION_IDLE);
    configure_wakenet(true);
    ESP_RETURN_ON_ERROR(baby_tank_face_set_standby(true), TAG,
                        "enter wakeable sleep failed");
    ESP_LOGI(TAG, "wakeable sleep requested by App");
    return ESP_OK;
}

static void remote_resume_task(void *argument)
{
    (void)argument;
    esp_err_t result = baby_tank_voice_runtime_set_remote_mode(false);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "remote lease resume failed: %s",
                 esp_err_to_name(result));
    }
    atomic_store(&s_remote_resume_pending, false);
    vTaskDelete(NULL);
}

static void remote_lease_expired(void *argument)
{
    (void)argument;
    if (!atomic_load(&s_remote_mode) ||
        atomic_exchange(&s_remote_resume_pending, true)) return;
    if (xTaskCreate(remote_resume_task, "remote_resume",
                    VOICE_REMOTE_RESUME_TASK_STACK, NULL,
                    VOICE_DIALOGUE_TASK_PRIORITY, NULL) != pdPASS) {
        atomic_store(&s_remote_resume_pending, false);
        ESP_LOGE(TAG, "unable to create remote resume task");
    }
}

static esp_err_t renew_remote_lease(void)
{
    if (s_remote_lease_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = remote_lease_expired,
            .name = "remote_lease",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_remote_lease_timer),
                            TAG, "remote lease timer create failed");
    }
    (void)esp_timer_stop(s_remote_lease_timer);
    return esp_timer_start_once(s_remote_lease_timer,
                                VOICE_REMOTE_MODE_LEASE_US);
}

esp_err_t baby_tank_voice_runtime_set_remote_mode(bool enabled)
{
    if (enabled && baby_tank_policy_is_locked()) return ESP_ERR_INVALID_STATE;
    if (s_mode_mutex == NULL) {
        s_mode_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mode_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                            "mode mutex allocation failed");
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mode_mutex, pdMS_TO_TICKS(10000)) ==
                            pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "mode transition is busy");
    esp_err_t result = ESP_OK;
    if (enabled) {
        atomic_store(&s_remote_mode_deadline_us,
                     esp_timer_get_time() + VOICE_REMOTE_MODE_LEASE_US);
        result = renew_remote_lease();
        if (result != ESP_OK || atomic_exchange(&s_remote_mode, true)) {
            xSemaphoreGive(s_mode_mutex);
            return result;
        }

        atomic_store(&s_explore_cancel, true);
        baby_tank_chassis_bridge_stop();
        (void)baby_tank_cloud_wait_text_idle(500U);
        baby_tank_cloud_cancel_current();
        baby_tank_audio_io_end_playback();
        set_phase(BABY_TANK_CONVERSATION_CANCELLED);
        set_phase(BABY_TANK_CONVERSATION_IDLE);
        (void)baby_tank_face_set_standby(false);
        result = baby_tank_voice_runtime_stop();
        if (result == ESP_OK) result = baby_tank_camera_stream_start();
        if (result != ESP_OK) {
            (void)baby_tank_camera_stream_stop();
            baby_tank_network_release_camera_hotspot();
            atomic_store(&s_remote_mode, false);
            (void)baby_tank_voice_runtime_start();
            (void)esp_timer_stop(s_remote_lease_timer);
        } else {
            ESP_LOGI(TAG,
                     "remote mode entered; voice memory released and camera started");
        }
        xSemaphoreGive(s_mode_mutex);
        return result;
    }

    atomic_store(&s_remote_mode_deadline_us, 0);
    if (!atomic_exchange(&s_remote_mode, false)) {
        xSemaphoreGive(s_mode_mutex);
        return ESP_OK;
    }
    if (s_remote_lease_timer != NULL) (void)esp_timer_stop(s_remote_lease_timer);
    result = baby_tank_camera_stream_stop();
    baby_tank_network_release_camera_hotspot();
    if (result == ESP_OK) result = baby_tank_voice_runtime_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "AI mode restore failed: %s", esp_err_to_name(result));
    }
    baby_tank_cloud_reset_cancel();
    set_phase(BABY_TANK_CONVERSATION_IDLE);
    (void)baby_tank_face_note_activity();
    ESP_LOGI(TAG, "remote mode exited; camera released and WakeNet restored");
    xSemaphoreGive(s_mode_mutex);
    return result;
}

static void show_explore_expression(const baby_tank_cloud_explore_step_t *step)
{
    baby_tank_expression_t expression;
    if (baby_tank_face_expression_from_name(step->emotion, &expression) != ESP_OK) return;
    const baby_tank_face_request_t request = {
        .expression = expression, .duration_ms = 3000U,
        .intensity = step->intensity,
        .priority = BABY_TANK_FACE_PRIORITY_INTERACTION,
    };
    baby_tank_face_submit(&request);
}

static void execute_exploration(const baby_tank_cloud_drive_action_t *action)
{
    atomic_store(&s_explore_cancel, false);
    atomic_store(&s_explore_active, true);
    uint32_t total_ms = 0U;
    esp_err_t result = baby_tank_chassis_bridge_stop();
    uint32_t epoch = baby_tank_chassis_bridge_control_epoch();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "AI group cannot verify chassis STOP: %s",
                 esp_err_to_name(result));
        goto done;
    }
    for (uint8_t index = 0U; index < 3U; ++index) {
        if (atomic_load(&s_explore_cancel) ||
            baby_tank_chassis_bridge_control_epoch() != epoch) break;
        uint8_t *jpeg = NULL;
        size_t jpeg_size = 0U;
        result = baby_tank_camera_stream_capture_jpeg(&jpeg, &jpeg_size);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "AI group camera failed step=%u: %s", index,
                     esp_err_to_name(result));
            break;
        }
        ESP_LOGI(TAG, "AI group photo trace=%s step=%u jpeg=%zu bytes",
                 action->trace_id, index, jpeg_size);
        baby_tank_cloud_explore_step_t decision = {0};
        result = baby_tank_cloud_explore_observe(action->trace_id, index,
                                                  jpeg, jpeg_size, &decision);
        free(jpeg);
        if (result != ESP_OK || atomic_load(&s_explore_cancel) ||
            baby_tank_chassis_bridge_control_epoch() != epoch) {
            ESP_LOGW(TAG, "AI group observation aborted step=%u: %s", index,
                     esp_err_to_name(result));
            break;
        }
        show_explore_expression(&decision);
        if (decision.light_mode != 0U) {
            result = baby_tank_chassis_bridge_light_mode(decision.light_mode);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "AI group RGB echo failed step=%u: %s", index,
                         esp_err_to_name(result));
                break;
            }
        }
        if (decision.stop) {
            ESP_LOGI(TAG, "AI group visual STOP trace=%s step=%u",
                     action->trace_id, index);
            break;
        }
        if (decision.duration_ms < 2000U || decision.duration_ms > 9000U ||
            total_ms + decision.duration_ms > 15000U ||
            baby_tank_chassis_bridge_control_epoch() != epoch ||
            atomic_load(&s_explore_cancel)) break;
        int16_t left = 0, right = 0;
        if (strcmp(decision.direction, "forward") == 0) {
            left = 850; right = 850;
        } else if (strcmp(decision.direction, "backward") == 0) {
            left = -850; right = -850;
        } else if (strcmp(decision.direction, "left") == 0) {
            left = -850; right = 850;
        } else if (strcmp(decision.direction, "right") == 0) {
            left = 850; right = -850;
        } else break;
        ESP_LOGI(TAG, "AI group drive trace=%s step=%u direction=%s duration=%u",
                 action->trace_id, index, decision.direction,
                 decision.duration_ms);
        result = baby_tank_chassis_bridge_pulse_guarded(
            left, right, decision.duration_ms, epoch);
        const char *outcome = result == ESP_OK ? "completed" :
                              result == ESP_ERR_INVALID_STATE ? "blocked" : "failed";
        const char *reason = result == ESP_OK ? "stop_echo_ok" :
                             result == ESP_ERR_INVALID_STATE ? "preempted_or_link_unverified" :
                             "uart_motion_or_stop_failed";
        bool finished = true;
        esp_err_t reported = baby_tank_cloud_explore_report(
            action->trace_id, index, decision.direction, outcome, reason,
            &finished);
        ESP_LOGI(TAG, "AI group result trace=%s step=%u result=%s report=%s",
                 action->trace_id, index, outcome, esp_err_to_name(reported));
        if (result != ESP_OK || reported != ESP_OK || finished) break;
        total_ms += decision.duration_ms;
        epoch = baby_tank_chassis_bridge_control_epoch();
    }
done:
    atomic_store(&s_explore_cancel, true);
    baby_tank_chassis_bridge_stop();
    atomic_store(&s_explore_active, false);
}

static void direct_action_group_task(void *argument)
{
    pcm_sink_t *sink = argument;
    const baby_tank_cloud_drive_action_t *action = sink->action;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!atomic_load(&s_remote_mode) &&
        !atomic_load(&s_explore_cancel) && action != NULL) {
        if (action->light_mode != 0U) {
            result = baby_tank_chassis_bridge_light_mode(action->light_mode);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "AI action RGB failed trace=%s mode=%u: %s",
                         action->trace_id, action->light_mode,
                         esp_err_to_name(result));
            }
        } else {
            result = ESP_OK;
        }
        if (result == ESP_OK && !atomic_load(&s_remote_mode) &&
            !atomic_load(&s_explore_cancel)) {
            int16_t speed = (int16_t)action->speed_per_mille;
            int16_t left = 0;
            int16_t right = 0;
            if (strcmp(action->direction, "forward") == 0) {
                left = speed; right = speed;
            } else if (strcmp(action->direction, "backward") == 0) {
                left = -speed; right = -speed;
            } else if (strcmp(action->direction, "left") == 0) {
                left = -speed; right = speed;
            } else if (strcmp(action->direction, "right") == 0) {
                left = speed; right = -speed;
            }
            ESP_LOGI(TAG,
                     "AI action group start trace=%s direction=%s duration=%u light=%u",
                     action->trace_id, action->direction,
                     action->duration_ms, action->light_mode);
            result = baby_tank_chassis_bridge_pulse_guarded(
                left, right, action->duration_ms, sink->action_epoch);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "AI action motion failed trace=%s: %s",
                         action->trace_id, esp_err_to_name(result));
            }
        }
    }
    sink->action_result = result;
    atomic_store(&sink->action_done, true);
    atomic_store(&s_explore_active, false);
    vTaskDelete(NULL);
}

static void start_pending_action_group(pcm_sink_t *sink)
{
    if (sink == NULL || sink->action == NULL ||
        !sink->action->approved || sink->action->explore ||
        sink->action->look || sink->action->draw ||
        atomic_load(&s_remote_mode)) return;
    if (atomic_exchange(&sink->action_started, true)) return;
    sink->action_result = ESP_ERR_INVALID_STATE;
    sink->action_epoch = baby_tank_chassis_bridge_control_epoch();
    atomic_store(&sink->action_done, false);
    atomic_store(&s_explore_cancel, false);
    atomic_store(&s_action_active, true);
    atomic_store(&s_explore_active, true);
    if (xTaskCreate(direct_action_group_task, "ai_action_group",
                    VOICE_ACTION_TASK_STACK, sink,
                    VOICE_ACTION_TASK_PRIORITY, NULL) != pdPASS) {
        sink->action_result = ESP_ERR_NO_MEM;
        atomic_store(&sink->action_done, true);
        atomic_store(&s_action_active, false);
        atomic_store(&s_explore_active, false);
        ESP_LOGE(TAG, "unable to start AI action group task");
    }
}

static esp_err_t wait_for_pending_action_group(pcm_sink_t *sink)
{
    if (sink == NULL) return ESP_ERR_INVALID_ARG;
    /* An ordinary answer has no chassis job; successful audio playback is
     * still a successfully completed turn. */
    if (!atomic_load(&sink->action_started)) return ESP_OK;
    uint32_t wait_ms = (uint32_t)sink->action->duration_ms + 2000U;
    while (!atomic_load(&sink->action_done) && wait_ms > 0U) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wait_ms = wait_ms > 20U ? wait_ms - 20U : 0U;
    }
    if (!atomic_load(&sink->action_done)) {
        atomic_store(&s_explore_cancel, true);
        (void)baby_tank_chassis_bridge_stop();
        for (uint8_t retry = 0U;
             retry < 50U && !atomic_load(&sink->action_done); ++retry) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    return atomic_load(&sink->action_done) ? sink->action_result
                                           : ESP_ERR_TIMEOUT;
}

esp_err_t baby_tank_voice_runtime_request_touch_reaction(
    baby_tank_touch_reaction_t side)
{
    ESP_RETURN_ON_FALSE(!baby_tank_policy_is_locked(), ESP_ERR_INVALID_STATE,
                        TAG, "side touch blocked by deep-night policy");
    ESP_RETURN_ON_FALSE(!atomic_load(&s_remote_mode), ESP_ERR_INVALID_STATE,
                        TAG, "touch AI is paused in remote mode");
    ESP_RETURN_ON_FALSE(side == BABY_TANK_TOUCH_REACTION_LEFT ||
                            side == BABY_TANK_TOUCH_REACTION_RIGHT,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid touch reaction side");
    ESP_RETURN_ON_FALSE(s_state_mutex != NULL && s_utterance_queue != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "voice runtime is not initialized");
    ESP_RETURN_ON_FALSE(take_state_lock(), ESP_ERR_TIMEOUT, TAG,
                        "voice state is busy");
    TickType_t now = xTaskGetTickCount();
    bool already_reacting = s_touch_reaction_active &&
                            s_state == VOICE_STATE_PROCESSING;
    if (s_state != VOICE_STATE_IDLE && !already_reacting) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (already_reacting &&
        uxQueueMessagesWaiting(s_utterance_queue) >=
            VOICE_TOUCH_REACTION_MAX_PENDING) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (s_last_touch_reaction != 0 &&
        now - s_last_touch_reaction <
            pdMS_TO_TICKS(VOICE_TOUCH_REACTION_COOLDOWN_MS)) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (s_last_touch_reaction == 0 ||
        now - s_last_touch_reaction >=
            pdMS_TO_TICKS(VOICE_TOUCH_REACTION_RESET_MS)) {
        s_touch_stage = 0U;
    }
    uint8_t touch_stage = s_touch_stage;
    voice_request_t request = {
        .kind = VOICE_REQUEST_TOUCH_REACTION,
        .turn_id = s_turn_id + 1U,
        .touch_side = side,
        .touch_stage = touch_stage,
    };
    if (xQueueSend(s_utterance_queue, &request, 0) != pdTRUE) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (s_touch_stage < 4U) ++s_touch_stage;
    s_last_touch_reaction = now;
    ++s_turn_id;
    s_touch_reaction_active = true;
    if (!already_reacting) {
        s_state = VOICE_STATE_PROCESSING;
        atomic_store(&s_capture_finished_us, esp_timer_get_time());
    }
    xSemaphoreGive(s_state_mutex);
    if (!already_reacting) {
        configure_wakenet(false);
        s_afe->reset_buffer(s_afe_data);
    }
    (void)baby_tank_face_set_standby(false);
    ESP_LOGI(TAG, "local %s-touch reaction turn=%lu stage=%u",
             side == BABY_TANK_TOUCH_REACTION_LEFT ? "left" : "right",
             (unsigned long)request.turn_id, (unsigned)(touch_stage + 1U));
    (void)baby_tank_interaction_memory_note_poke();
    return ESP_OK;
}

static void append_samples(const int16_t *samples, size_t count)
{
    if (samples == NULL || count == 0U) return;
    size_t available = VOICE_MAX_UTTERANCE_SAMPLES - s_recorded_samples;
    if (count > available) count = available;
    memcpy(s_recording + s_recorded_samples, samples,
           count * sizeof(*samples));
    s_recorded_samples += count;
}

static void finish_recording(void)
{
    if (!take_state_lock()) return;
    if (s_state != VOICE_STATE_LISTENING || !s_speech_started ||
        s_recorded_samples == 0U) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    s_state = VOICE_STATE_PROCESSING;
    size_t sample_count = s_recorded_samples;
    xSemaphoreGive(s_state_mutex);
    set_phase(BABY_TANK_CONVERSATION_THINKING);
    ESP_LOGI(TAG, "captured utterance: %u ms peak_level=%lu vad_silence_ms=%lu quiet_ms=%lu",
             (unsigned)(sample_count * 1000U /
                        BABY_TANK_AUDIO_SAMPLE_RATE),
             (unsigned long)s_peak_frame_level,
             (unsigned long)s_silence_ms,
             (unsigned long)s_energy_quiet_ms);
}

static uint32_t frame_mean_abs(const afe_fetch_result_t *result)
{
    if (result->data == NULL || result->data_size <= 0) return 0U;
    size_t count = (size_t)result->data_size / sizeof(int16_t);
    uint64_t sum = 0U;
    for (size_t index = 0; index < count; ++index) {
        int32_t sample = result->data[index];
        sum += (uint32_t)(sample < 0 ? -sample : sample);
    }
    return count > 0U ? (uint32_t)(sum / count) : 0U;
}

static void process_listening_frame(const afe_fetch_result_t *result)
{
    uint32_t duration_ms = frame_duration_ms(result->data_size);
    uint32_t frame_level = frame_mean_abs(result);
    bool finish = false;
    bool timeout = false;
    bool just_started = false;
    uint32_t turn_id = 0U;
    if (!take_state_lock()) return;
    if (s_state != VOICE_STATE_LISTENING) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    if (esp_timer_get_time() < s_ignore_capture_until_us) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    if (!s_speech_started) {
        s_waited_ms += duration_ms;
        if (result->vad_state == VAD_SPEECH) {
            s_speech_started = true;
            just_started = true;
            turn_id = s_turn_id;
            s_silence_ms = 0U;
            s_peak_frame_level = frame_level;
            if (result->vad_cache != NULL && result->vad_cache_size > 0) {
                append_samples(result->vad_cache,
                               (size_t)result->vad_cache_size /
                                   sizeof(int16_t));
            }
            append_samples(result->data,
                           (size_t)result->data_size / sizeof(int16_t));
        } else if (esp_timer_get_time() >= s_listen_deadline_us) {
            s_session_active = false;
            timeout = true;
        }
    } else {
        append_samples(result->data,
                       (size_t)result->data_size / sizeof(int16_t));
        if (result->vad_state == VAD_SPEECH) {
            s_silence_ms = 0U;
        } else {
            s_silence_ms += duration_ms;
        }
        if (frame_level > s_peak_frame_level) s_peak_frame_level = frame_level;
        /* AFE VAD can remain in SPEECH on steady background noise. End a
         * real utterance once the cleaned PCM is quiet relative to that
         * utterance, without shortening softly spoken questions. */
        uint32_t energy_floor = s_peak_frame_level / 8U;
        if (energy_floor < 250U) energy_floor = 250U;
        if (s_peak_frame_level >= 700U && frame_level < energy_floor) {
            s_energy_quiet_ms += duration_ms;
        } else {
            s_energy_quiet_ms = 0U;
        }
        finish = s_silence_ms >= VOICE_END_SILENCE_MS ||
                 (s_recorded_samples >=
                      BABY_TANK_AUDIO_SAMPLE_RATE *
                          VOICE_ENERGY_MIN_SPEECH_MS / 1000U &&
                  s_energy_quiet_ms >= VOICE_ENERGY_END_SILENCE_MS) ||
                 s_recorded_samples >= VOICE_MAX_UTTERANCE_SAMPLES;
        /* Keep a very short utterance open long enough to reject an isolated
         * noise spike, but never force a child who has finished speaking to
         * fill a four-second recording. The no-speech listening deadline is
         * independent and remains open for VOICE_WAIT_FOR_SPEECH_MS. */
        if (s_recorded_samples <
                BABY_TANK_AUDIO_SAMPLE_RATE * VOICE_MIN_UTTERANCE_MS / 1000U &&
            s_recorded_samples < VOICE_MAX_UTTERANCE_SAMPLES) {
            finish = false;
        }
    }
    xSemaphoreGive(s_state_mutex);

    if (just_started) {
        set_phase(BABY_TANK_CONVERSATION_USER_SPEAKING);
        const voice_request_t utterance = {
            .kind = VOICE_REQUEST_RECORDED_SPEECH,
            .turn_id = turn_id,
        };
        if (xQueueSend(s_utterance_queue, &utterance, 0) != pdTRUE) {
            ESP_LOGE(TAG, "unable to start streaming microphone upload");
            complete_turn();
            return;
        }
    }
    if (timeout) {
        ESP_LOGI(TAG, "listening timed out without speech");
        complete_turn();
    } else if (finish) {
        finish_recording();
    }
}

static void voice_feed_task(void *argument)
{
    (void)argument;
    int chunk_samples = s_afe->get_feed_chunksize(s_afe_data);
    int16_t *samples = heap_caps_malloc(
        (size_t)chunk_samples * sizeof(*samples),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (samples == NULL) {
        ESP_LOGE(TAG, "unable to allocate AFE feed buffer");
        s_feed_task = NULL;
        vTaskDelete(NULL);
    }
    while (!atomic_load(&s_runtime_stopping)) {
        if (baby_tank_audio_io_is_playing()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        esp_err_t result = baby_tank_audio_io_read(samples,
                                                   (size_t)chunk_samples);
        if (result != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        s_afe->feed(s_afe_data, samples);
    }
    free(samples);
    s_feed_task = NULL;
    vTaskDelete(NULL);
}

static void voice_fetch_task(void *argument)
{
    (void)argument;
    while (!atomic_load(&s_runtime_stopping)) {
        if (baby_tank_audio_io_is_playing()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        afe_fetch_result_t *result = s_afe->fetch(s_afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch failed");
            continue;
        }
        if (atomic_load(&s_wake_ack_busy)) continue;
        if (atomic_load(&s_remote_mode)) {
            int64_t deadline = atomic_load(&s_remote_mode_deadline_us);
            if (deadline > 0 && esp_timer_get_time() >= deadline) {
                ESP_LOGW(TAG, "remote mode lease expired");
                (void)baby_tank_voice_runtime_set_remote_mode(false);
            } else {
                continue;
            }
        }
        voice_state_t state = VOICE_STATE_IDLE;
        if (take_state_lock()) {
            state = s_state;
            xSemaphoreGive(s_state_mutex);
        }
        if (state == VOICE_STATE_IDLE &&
            result->wakeup_state == WAKENET_DETECTED &&
            !baby_tank_policy_is_locked()) {
            ESP_LOGI(TAG, "WakeNet detected 喵喵同学");
            baby_tank_voice_runtime_request_listen();
        } else if (state == VOICE_STATE_LISTENING) {
            process_listening_frame(result);
        }
    }
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t write_pcm_chunked(pcm_sink_t *sink, const int16_t *samples,
                                   size_t sample_count)
{
    for (size_t offset = 0U; offset < sample_count;) {
        if (cancellation_requested()) return ESP_ERR_INVALID_STATE;
        while (sink->next_face_cue < sink->face_cue_count &&
            sink->written_samples >=
                sink->face_cues[sink->next_face_cue].sample_offset) {
            const face_cue_t *cue = &sink->face_cues[sink->next_face_cue++];
            ESP_RETURN_ON_ERROR(baby_tank_conversation_set_ai_emotion(
                                    cue->emotion, cue->intensity),
                                TAG, "unable to switch sentence expression");
            ESP_LOGI(TAG, "TTS face cue sample=%zu emotion=%s intensity=%u",
                     cue->sample_offset, cue->emotion, cue->intensity);
        }
        size_t count = sample_count - offset;
        if (count > VOICE_PCM_PLAYBACK_CHUNK_SAMPLES) {
            count = VOICE_PCM_PLAYBACK_CHUNK_SAMPLES;
        }
        if (sink->next_face_cue < sink->face_cue_count) {
            size_t until_cue = sink->face_cues[sink->next_face_cue].sample_offset -
                               sink->written_samples;
            if (until_cue < count) count = until_cue;
        }
        ESP_RETURN_ON_ERROR(
            baby_tank_conversation_observe_tts_pcm16(samples + offset, count),
            TAG, "unable to animate TTS PCM");
        int64_t write_started_us = esp_timer_get_time();
        esp_err_t result = baby_tank_audio_io_write(
            (int16_t *)(samples + offset), count);
        sink->codec_write_us += esp_timer_get_time() - write_started_us;
        ESP_RETURN_ON_ERROR(result, TAG, "unable to play TTS PCM");
        offset += count;
        sink->written_samples += count;
    }
    return ESP_OK;
}

static esp_err_t play_pcm_samples(pcm_sink_t *sink, const int16_t *samples,
                                  size_t sample_count)
{
    if (sample_count == 0U) return ESP_OK;
    size_t offset = 0U;
    if (!sink->playback_started && sink->prebuffer != NULL) {
        size_t available = VOICE_PCM_PREBUFFER_SAMPLES -
                           sink->prebuffered_samples;
        size_t copy_count = sample_count < available ? sample_count : available;
        memcpy(sink->prebuffer + sink->prebuffered_samples, samples,
               copy_count * sizeof(*samples));
        sink->prebuffered_samples += copy_count;
        offset += copy_count;
        if (sink->prebuffered_samples < VOICE_PCM_PREBUFFER_SAMPLES) {
            return ESP_OK;
        }
    }

    if (!sink->playback_started) {
        sink->playback_started = true;
        sink->first_playback_us = esp_timer_get_time();
        int64_t capture_finished = atomic_load(&s_capture_finished_us);
        if (capture_finished > 0) {
            ESP_LOGI(TAG, "reply first PCM after capture end=%lld ms",
                     (long long)((sink->first_playback_us - capture_finished) / 1000));
        }
        set_phase(BABY_TANK_CONVERSATION_AI_SPEAKING);
        start_pending_action_group(sink);
        if (sink->prebuffered_samples > 0U) {
            ESP_RETURN_ON_ERROR(
                write_pcm_chunked(sink, sink->prebuffer,
                                  sink->prebuffered_samples), TAG,
                                "unable to play prebuffered TTS PCM");
            sink->prebuffered_samples = 0U;
        }
    }

    if (offset < sample_count) {
        ESP_RETURN_ON_ERROR(
            write_pcm_chunked(sink, samples + offset, sample_count - offset),
            TAG, "unable to play TTS PCM");
    }
    return ESP_OK;
}

static esp_err_t flush_pcm_prebuffer(pcm_sink_t *sink)
{
    if (sink->prebuffered_samples == 0U) return ESP_OK;
    if (!sink->playback_started) {
        sink->playback_started = true;
        sink->first_playback_us = esp_timer_get_time();
        int64_t capture_finished = atomic_load(&s_capture_finished_us);
        if (capture_finished > 0) {
            ESP_LOGI(TAG, "reply first PCM after capture end=%lld ms",
                     (long long)((sink->first_playback_us - capture_finished) / 1000));
        }
        set_phase(BABY_TANK_CONVERSATION_AI_SPEAKING);
        start_pending_action_group(sink);
    }
    esp_err_t result = write_pcm_chunked(sink, sink->prebuffer,
                                         sink->prebuffered_samples);
    sink->prebuffered_samples = 0U;
    return result;
}

static void pcm_player_task(void *argument)
{
    pcm_sink_t *sink = argument;
    uint8_t raw[1024];
    int16_t samples[513];
    bool has_tail = false;
    uint8_t tail = 0U;
    esp_err_t result = ESP_OK;
    while (!atomic_load(&sink->player_abort) && !cancellation_requested()) {
        size_t bytes = xStreamBufferReceive(sink->stream, raw, sizeof(raw),
                                            pdMS_TO_TICKS(100));
        if (bytes == 0U) {
            if (atomic_load(&sink->producer_done)) break;
            if (sink->playback_started) ++sink->ring_empty_waits;
            continue;
        }
        size_t offset = 0U;
        size_t count = 0U;
        if (has_tail) {
            uint8_t pair[2] = {tail, raw[0]};
            memcpy(samples, pair, sizeof(pair));
            has_tail = false;
            offset = 1U;
            count = 1U;
        }
        size_t even_bytes = (bytes - offset) & ~(size_t)1U;
        if (even_bytes > 0U) {
            memcpy(samples + count, raw + offset, even_bytes);
            count += even_bytes / sizeof(int16_t);
            offset += even_bytes;
        }
        if (offset < bytes) {
            tail = raw[offset];
            has_tail = true;
        }
        result = play_pcm_samples(sink, samples, count);
        if (result != ESP_OK) break;
    }
    if (result == ESP_OK &&
        (atomic_load(&sink->player_abort) || cancellation_requested())) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK && has_tail) result = ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK) result = flush_pcm_prebuffer(sink);
    sink->player_stack_spare_bytes = uxTaskGetStackHighWaterMark(NULL);
    sink->player_result = result;
    TaskHandle_t dialogue_task = sink->dialogue_task;
    atomic_store(&sink->player_finished, true);
    xTaskNotifyGive(dialogue_task);
    vTaskSuspend(NULL);
}

static void start_psram_pcm_player(pcm_sink_t *sink)
{
    sink->stream_storage = heap_caps_malloc(VOICE_PCM_RING_BYTES + 1U,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sink->stream_storage == NULL) return;
    sink->stream = xStreamBufferCreateStatic(
        VOICE_PCM_RING_BYTES + 1U, 1U, sink->stream_storage,
        &sink->stream_control);
    if (sink->stream == NULL) {
        free(sink->stream_storage);
        sink->stream_storage = NULL;
        return;
    }
    sink->dialogue_task = xTaskGetCurrentTaskHandle();
    if (xTaskCreateWithCaps(pcm_player_task, "pcm_player",
                            VOICE_PCM_PLAYER_STACK, sink,
                            VOICE_PCM_PLAYER_PRIORITY, &sink->player_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vStreamBufferDelete(sink->stream);
        free(sink->stream_storage);
        sink->stream = NULL;
        sink->stream_storage = NULL;
        ESP_LOGW(TAG, "PSRAM PCM player task unavailable; direct playback fallback");
        return;
    }
    ESP_LOGI(TAG, "PSRAM PCM ring enabled capacity=%u bytes prebuffer=%u bytes",
             VOICE_PCM_RING_BYTES,
             VOICE_PCM_PREBUFFER_SAMPLES * sizeof(int16_t));
}

static esp_err_t finish_psram_pcm_player(pcm_sink_t *sink, bool abort)
{
    if (sink->stream == NULL) return ESP_OK;
    atomic_store(&sink->player_abort, abort);
    atomic_store(&sink->producer_done, true);
    while (!atomic_load(&sink->player_finished)) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    esp_err_t result = sink->player_result;
    ESP_LOGI(TAG, "PSRAM PCM ring peak=%zu bytes empty_waits=%lu stack_spare=%u bytes result=%s",
             sink->ring_peak_bytes, (unsigned long)sink->ring_empty_waits,
             (unsigned)sink->player_stack_spare_bytes,
             esp_err_to_name(result));
    vTaskDeleteWithCaps(sink->player_task);
    vStreamBufferDelete(sink->stream);
    free(sink->stream_storage);
    sink->stream = NULL;
    sink->stream_storage = NULL;
    return result;
}

static esp_err_t microphone_pcm_source(uint8_t *buffer, size_t capacity,
                                       size_t *size, void *context)
{
    pcm_sink_t *sink = context;
    ESP_RETURN_ON_FALSE(buffer != NULL && size != NULL && sink != NULL &&
                            capacity >= sizeof(int16_t),
                        ESP_ERR_INVALID_ARG, TAG, "invalid microphone stream buffer");
    const int64_t deadline_us = esp_timer_get_time() + 20000000LL;
    while (true) {
        if (!take_state_lock()) return ESP_ERR_TIMEOUT;
        bool active = s_state == VOICE_STATE_LISTENING ||
                      s_state == VOICE_STATE_PROCESSING;
        bool finished = s_state == VOICE_STATE_PROCESSING;
        if (sink->uploaded_samples > s_recorded_samples) {
            xSemaphoreGive(s_state_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        size_t available = s_recorded_samples - sink->uploaded_samples;
        size_t limit = capacity / sizeof(int16_t);
        /* During capture, wait for one full transport frame instead of
         * sending whichever small AFE fragment happened to arrive first.
         * Once VAD closes the utterance, immediately flush the final partial
         * frame and then return the zero-length end marker. */
        if (active && available > 0U && (available >= limit || finished)) {
            size_t count = available;
            if (count > limit) count = limit;
            memcpy(buffer, s_recording + sink->uploaded_samples,
                   count * sizeof(int16_t));
            sink->uploaded_samples += count;
            *size = count * sizeof(int16_t);
            xSemaphoreGive(s_state_mutex);
            return ESP_OK;
        }
        xSemaphoreGive(s_state_mutex);
        if (!active || cancellation_requested()) return ESP_ERR_INVALID_STATE;
        if (finished) {
            *size = 0U;
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline_us) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t pcm_tts_sink(const uint8_t *data, size_t size,
                              baby_tank_cloud_audio_format_t format,
                              uint32_t sample_rate, void *context)
{
    pcm_sink_t *sink = context;
    int64_t arrived_us = esp_timer_get_time();
    if (sink->last_callback_end_us > 0) {
        int64_t gap_us = arrived_us - sink->last_callback_end_us;
        if (gap_us > sink->max_source_gap_us) sink->max_source_gap_us = gap_us;
        if (gap_us > 100000) ++sink->source_gaps_over_100ms;
    }
    if (sink->playback_started) {
        int64_t delivered_ms = (int64_t)(sink->received_bytes / 32U);
        int64_t elapsed_ms = (arrived_us - sink->first_playback_us) / 1000;
        int64_t headroom_ms = delivered_ms - elapsed_ms;
        if (headroom_ms < sink->min_playback_headroom_ms) {
            sink->min_playback_headroom_ms = headroom_ms;
        }
    }
    if (cancellation_requested()) return ESP_ERR_INVALID_STATE;
    if (format != BABY_TANK_CLOUD_AUDIO_PCM_S16LE ||
        sample_rate != BABY_TANK_AUDIO_SAMPLE_RATE) {
        ESP_LOGE(TAG, "unsupported TTS audio format=%d rate=%lu",
                 format, (unsigned long)sample_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sink->stream != NULL) {
        size_t offset = 0U;
        while (offset < size) {
            if (cancellation_requested() || atomic_load(&sink->player_finished)) {
                return ESP_ERR_INVALID_STATE;
            }
            offset += xStreamBufferSend(sink->stream, data + offset,
                                        size - offset, pdMS_TO_TICKS(100));
            size_t queued = xStreamBufferBytesAvailable(sink->stream);
            if (queued > sink->ring_peak_bytes) sink->ring_peak_bytes = queued;
        }
        sink->received_bytes += size;
        sink->last_callback_end_us = esp_timer_get_time();
        return ESP_OK;
    }
    sink->received_bytes += size;

    int16_t staging[VOICE_PCM_STAGING_SAMPLES];
    size_t offset = 0U;
    if (sink->has_tail && size > 0U) {
        uint8_t pair[2] = {sink->tail, data[0]};
        memcpy(staging, pair, sizeof(pair));
        sink->has_tail = false;
        offset = 1U;
        ESP_RETURN_ON_ERROR(play_pcm_samples(sink, staging, 1U), TAG,
                            "unable to buffer TTS PCM");
    }
    while (offset + 1U < size) {
        size_t bytes = size - offset;
        size_t capacity = sizeof(staging);
        if (bytes > capacity) bytes = capacity;
        bytes &= ~(size_t)1U;
        memcpy(staging, data + offset, bytes);
        size_t sample_count = bytes / sizeof(int16_t);
        ESP_RETURN_ON_ERROR(play_pcm_samples(sink, staging, sample_count), TAG,
                            "unable to buffer TTS PCM");
        offset += bytes;
        if (cancellation_requested()) return ESP_ERR_INVALID_STATE;
    }
    if (offset < size) {
        sink->tail = data[offset];
        sink->has_tail = true;
    }
    sink->last_callback_end_us = esp_timer_get_time();
    return ESP_OK;
}

static bool transition_to_speaking(void)
{
    if (!take_state_lock()) return false;
    bool allowed = s_state == VOICE_STATE_PROCESSING && !s_cancel_requested;
    if (allowed) s_state = VOICE_STATE_SPEAKING;
    xSemaphoreGive(s_state_mutex);
    return allowed;
}

static bool parse_face_cues(const char *header, const char *first_emotion,
                            uint8_t first_intensity, pcm_sink_t *sink)
{
    if (header == NULL || header[0] == '\0') return false;
    const char *cursor = header;
    uint8_t count = 0U;
    size_t previous_offset = 0U;
    while (*cursor != '\0' && count < VOICE_FACE_CUE_LIMIT) {
        if (*cursor < '0' || *cursor > '9') return false;
        char *end = NULL;
        unsigned long sample_offset = strtoul(cursor, &end, 10);
        if (end == cursor || *end != ':' || sample_offset > 1048576UL ||
            (count == 0U && sample_offset != 0UL) ||
            (count > 0U && sample_offset <= previous_offset)) return false;
        cursor = end + 1;
        const char *emotion_end = strchr(cursor, ':');
        if (emotion_end == NULL || emotion_end == cursor ||
            (size_t)(emotion_end - cursor) >= sizeof(sink->face_cues[0].emotion)) {
            return false;
        }
        face_cue_t *cue = &sink->face_cues[count];
        memcpy(cue->emotion, cursor, (size_t)(emotion_end - cursor));
        cue->emotion[emotion_end - cursor] = '\0';
        baby_tank_expression_t validated;
        if (baby_tank_face_expression_from_name(cue->emotion, &validated) != ESP_OK) {
            return false;
        }
        cursor = emotion_end + 1;
        if (*cursor < '0' || *cursor > '9') return false;
        unsigned long intensity = strtoul(cursor, &end, 10);
        if (end == cursor || intensity < 20UL || intensity > 100UL ||
            (*end != ',' && *end != '\0') ||
            (*end == ',' && end[1] == '\0')) return false;
        cue->sample_offset = (size_t)sample_offset;
        cue->intensity = (uint8_t)intensity;
        previous_offset = cue->sample_offset;
        ++count;
        cursor = *end == ',' ? end + 1 : end;
    }
    if (*cursor != '\0' || count == 0U ||
        strcmp(sink->face_cues[0].emotion, first_emotion) != 0 ||
        sink->face_cues[0].intensity != first_intensity) return false;
    sink->face_cue_count = count;
    sink->next_face_cue = 1U;
    return true;
}

static esp_err_t conversation_metadata(const char *emotion,
                                       uint8_t intensity,
                                       const char *face_cues,
                                       const char *mood,
                                       uint8_t mood_intensity,
                                       const char *thought,
                                       const char *thought_text,
                                       void *context)
{
    pcm_sink_t *sink = context;
    if (sink != NULL && face_cues != NULL && face_cues[0] != '\0' &&
        !parse_face_cues(face_cues, emotion, intensity, sink)) {
        sink->face_cue_count = 0U;
        sink->next_face_cue = 0U;
        ESP_LOGW(TAG, "invalid TTS face cue timeline; using one emotion");
    }
    ESP_RETURN_ON_FALSE(transition_to_speaking(), ESP_ERR_INVALID_STATE, TAG,
                        "conversation is no longer active");
    (void)baby_tank_face_set_standby(false);
    esp_err_t mood_result = baby_tank_face_set_mood(mood, mood_intensity);
    if (mood_result != ESP_OK) {
        ESP_LOGW(TAG, "AI mood rejected: %s", esp_err_to_name(mood_result));
    }
    esp_err_t thought_result = baby_tank_face_set_thought(thought);
    if (thought_result != ESP_OK) {
        ESP_LOGW(TAG, "AI thought rejected: %s", esp_err_to_name(thought_result));
    }
    if (thought_text != NULL && thought_text[0] != '\0') {
        esp_err_t text_result = baby_tank_face_set_thought_text(thought_text);
        if (text_result != ESP_OK) {
            ESP_LOGW(TAG, "AI dream caption rejected: %s",
                     esp_err_to_name(text_result));
        }
    }
    set_phase(BABY_TANK_CONVERSATION_THINKING);
    esp_err_t result = baby_tank_conversation_set_ai_emotion(
        emotion, intensity);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "AI emotion rejected: %s", esp_err_to_name(result));
    }
    return baby_tank_audio_io_begin_playback();
}

static esp_err_t conversation_face_cue(size_t sample_offset,
                                        const char *emotion,
                                        uint8_t intensity, void *context)
{
    pcm_sink_t *sink = context;
    baby_tank_expression_t validated;
    uint8_t count = sink != NULL ? atomic_load(&sink->face_cue_count) : 0U;
    ESP_RETURN_ON_FALSE(sink != NULL && emotion != NULL &&
                            count < VOICE_FACE_CUE_LIMIT &&
                            sample_offset <= 1048576U &&
                            intensity >= 20U && intensity <= 100U &&
                            baby_tank_face_expression_from_name(
                                emotion, &validated) == ESP_OK,
                        ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid streamed TTS face cue");
    if (count > 0U) {
        ESP_RETURN_ON_FALSE(sample_offset > sink->face_cues[
                                count - 1U].sample_offset,
                            ESP_ERR_INVALID_RESPONSE, TAG,
                            "out-of-order TTS face cue");
    }
    face_cue_t *cue = &sink->face_cues[count];
    cue->sample_offset = sample_offset;
    snprintf(cue->emotion, sizeof(cue->emotion), "%s", emotion);
    cue->intensity = intensity;
    atomic_store(&sink->face_cue_count, count + 1U);
    return ESP_OK;
}

static void dialogue_failure(const char *operation, esp_err_t result)
{
    if (cancellation_requested()) {
        ESP_LOGI(TAG, "%s cancelled", operation);
    } else {
        ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(result));
        ESP_LOGW(TAG, "cloud failure heap: internal_free=%zu internal_largest=%zu psram_free=%zu",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        set_phase(BABY_TANK_CONVERSATION_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    complete_turn();
}

static void dialogue_task(void *argument)
{
    (void)argument;
    voice_request_t utterance;
    while (xQueueReceive(s_utterance_queue, &utterance, portMAX_DELAY) ==
           pdTRUE) {
        if (atomic_load(&s_runtime_stopping)) break;
        atomic_store(&s_dialogue_busy, true);
        const bool typed_turn = utterance.kind == VOICE_REQUEST_CLOUD_TEXT;
        const bool user_turn = utterance.kind == VOICE_REQUEST_RECORDED_SPEECH ||
                               typed_turn;
        if (cancellation_requested()) {
            if (typed_turn) (void)baby_tank_cloud_complete_text_turn(
                utterance.text_job_id, "blocked");
            complete_turn();
            goto dialogue_next;
        }
        /* Side touch is a local personality blind box. The first two touches
         * draw non-repeating left/right reactions (with a rare surprise);
         * a rapid third touch starts the bounded "please stop" escalation.
         * It must stay independent of Wi-Fi, LLM and cloud TTS. */
        if (utterance.kind == VOICE_REQUEST_TOUCH_REACTION) {
            esp_err_t touch_result = play_touch_reaction(
                utterance.touch_side, utterance.touch_stage);
            if (touch_result != ESP_OK) {
                if (take_state_lock()) {
                    s_touch_reaction_active = false;
                    xSemaphoreGive(s_state_mutex);
                }
                xQueueReset(s_utterance_queue);
                dialogue_failure("local touch reaction", touch_result);
            } else {
                bool pending = false;
                if (take_state_lock()) {
                    pending = uxQueueMessagesWaiting(s_utterance_queue) > 0U;
                    if (!pending) s_touch_reaction_active = false;
                    xSemaphoreGive(s_state_mutex);
                }
                if (!pending) {
                    set_phase(BABY_TANK_CONVERSATION_IDLE);
                    complete_turn();
                }
            }
            goto dialogue_next;
        }
        baby_tank_cloud_reset_cancel();
        if (!baby_tank_network_cloud_is_ready()) {
            if (typed_turn) (void)baby_tank_cloud_complete_text_turn(
                utterance.text_job_id, "failed");
            dialogue_failure("cloud credentials", ESP_ERR_INVALID_STATE);
            goto dialogue_next;
        }
        esp_err_t result = baby_tank_network_wait_connected(
            VOICE_NETWORK_WAIT_MS);
        if (result != ESP_OK) {
            if (typed_turn) (void)baby_tank_cloud_complete_text_turn(
                utterance.text_job_id, "failed");
            dialogue_failure("network connection", result);
            goto dialogue_next;
        }

        {
            baby_tank_cloud_drive_action_t drive_action = {0};
            pcm_sink_t sink = {
                .min_playback_headroom_ms = INT64_MAX,
                .action = &drive_action,
            };
            sink.prebuffer = heap_caps_malloc(
                VOICE_PCM_PREBUFFER_SAMPLES * sizeof(*sink.prebuffer),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (sink.prebuffer == NULL) {
                sink.prebuffer = heap_caps_malloc(
                    VOICE_PCM_PREBUFFER_SAMPLES * sizeof(*sink.prebuffer),
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            start_psram_pcm_player(&sink);
            baby_tank_cloud_audio_format_t format =
                BABY_TANK_CLOUD_AUDIO_UNKNOWN;
            uint32_t sample_rate = 0U;
            int64_t started_us = esp_timer_get_time();
            if (utterance.kind == VOICE_REQUEST_RECORDED_SPEECH) {
                result = baby_tank_cloud_converse_pcm16_stream(
                    microphone_pcm_source,
                    conversation_metadata, conversation_face_cue,
                    pcm_tts_sink, &sink, &format,
                    &sample_rate, &drive_action);
            } else if (typed_turn) {
                result = baby_tank_cloud_converse_text_turn(
                    utterance.text_job_id, conversation_metadata,
                    conversation_face_cue, pcm_tts_sink, &sink, &format,
                    &sample_rate, &drive_action);
            } else {
                result = baby_tank_cloud_converse_touch(
                    utterance.touch_side == BABY_TANK_TOUCH_REACTION_LEFT,
                    conversation_metadata, pcm_tts_sink, &sink, &format,
                    &sample_rate);
            }
            if (user_turn && result == ESP_OK && drive_action.approved &&
                !drive_action.explore && !drive_action.look && !drive_action.draw) {
                /* HTTP headers and WebSocket reply metadata can become
                 * visible after the first-PCM hook. Start once here as a
                 * bounded fallback; start_pending_action_group is idempotent. */
                if (!atomic_load(&sink.action_started)) {
                    ESP_LOGW(TAG,
                             "AI drive metadata arrived after first PCM trace=%s",
                             drive_action.trace_id);
                    start_pending_action_group(&sink);
                }
                if (!atomic_exchange(&sink.action_started, true)) {
                    sink.action_result = ESP_ERR_INVALID_STATE;
                    atomic_store(&sink.action_done, true);
                    ESP_LOGW(TAG, "AI drive blocked before start trace=%s",
                             drive_action.trace_id);
                }
            }
            if (user_turn) {
                if (result == ESP_OK && drive_action.approved &&
                    drive_action.look) {
                    uint8_t *jpeg = NULL;
                    size_t jpeg_size = 0U;
                    result = baby_tank_camera_stream_capture_jpeg(
                        &jpeg, &jpeg_size);
                    if (result == ESP_OK) {
                        ESP_LOGI(TAG, "AI look trace=%s jpeg=%zu bytes",
                                 drive_action.trace_id, jpeg_size);
                        result = baby_tank_cloud_vision_describe(
                            drive_action.trace_id, jpeg, jpeg_size,
                            conversation_metadata, pcm_tts_sink, &sink,
                            &format, &sample_rate);
                    }
                    free(jpeg);
                }
            }
            if (result == ESP_OK &&
                (sink.has_tail || sink.received_bytes % sizeof(int16_t) != 0U)) {
                result = ESP_ERR_INVALID_SIZE;
            }
            if (result == ESP_OK &&
                (format != BABY_TANK_CLOUD_AUDIO_PCM_S16LE ||
                 sample_rate != BABY_TANK_AUDIO_SAMPLE_RATE)) {
                result = ESP_ERR_NOT_SUPPORTED;
            }
            if (sink.stream != NULL) {
                esp_err_t playback_result = finish_psram_pcm_player(
                    &sink, result != ESP_OK);
                if (result == ESP_OK) result = playback_result;
            } else if (result == ESP_OK) {
                result = flush_pcm_prebuffer(&sink);
            }
            if (result == ESP_OK && sink.playback_started) {
                int64_t expected_us =
                    (int64_t)sink.received_bytes * 1000000LL /
                    (sizeof(int16_t) * BABY_TANK_AUDIO_SAMPLE_RATE);
                int64_t remaining_us = expected_us -
                    (esp_timer_get_time() - sink.first_playback_us);
                if (remaining_us > 0) {
                    if (remaining_us > 1200000LL) remaining_us = 1200000LL;
                    vTaskDelay(pdMS_TO_TICKS((remaining_us + 999LL) / 1000LL));
                }
            }
            ESP_LOGI(TAG, "cloud turn=%lu PCM bytes=%zu rate=%lu expected_duration=%zu ms codec_write=%lld ms total_elapsed=%lld ms",
                     (unsigned long)utterance.turn_id, sink.received_bytes,
                     (unsigned long)sample_rate,
                     sink.received_bytes * 1000U /
                         (sizeof(int16_t) * BABY_TANK_AUDIO_SAMPLE_RATE),
                     (long long)(sink.codec_write_us / 1000),
                     (long long)((esp_timer_get_time() - started_us) / 1000));
            ESP_LOGI(TAG, "PCM source max_gap=%lld ms gaps_over_100ms=%lu min_playback_headroom=%lld ms",
                     (long long)(sink.max_source_gap_us / 1000),
                     (unsigned long)sink.source_gaps_over_100ms,
                     (long long)(sink.min_playback_headroom_ms == INT64_MAX
                                     ? 0 : sink.min_playback_headroom_ms));
            free(sink.prebuffer);
            if (sink.playback_started) {
                vTaskDelay(pdMS_TO_TICKS(VOICE_PLAYBACK_TAIL_MS));
            }
            baby_tank_audio_io_end_playback();
            if (utterance.kind == VOICE_REQUEST_TOUCH_REACTION &&
                take_state_lock()) {
                s_touch_reaction_active = false;
                xSemaphoreGive(s_state_mutex);
            }
            if (result != ESP_OK && atomic_load(&sink.action_started)) {
                atomic_store(&s_explore_cancel, true);
                (void)baby_tank_chassis_bridge_stop();
            }
            if (atomic_load(&sink.action_started)) {
                enter_action_state("drive");
            }
            esp_err_t action_result = wait_for_pending_action_group(&sink);
            if (result == ESP_ERR_NOT_FOUND &&
                utterance.kind == VOICE_REQUEST_RECORDED_SPEECH) {
                ESP_LOGI(TAG, "ASR heard no words; ignoring ambient sound");
                if (take_state_lock()) {
                    if (esp_timer_get_time() >= s_listen_deadline_us) {
                        s_session_active = false;
                    }
                    xSemaphoreGive(s_state_mutex);
                }
                complete_turn();
                goto dialogue_next;
            }
            if (result != ESP_OK) {
                if (typed_turn) (void)baby_tank_cloud_complete_text_turn(
                    utterance.text_job_id,
                    cancellation_requested() ? "blocked" : "failed");
                dialogue_failure("cloud conversation", result);
                goto dialogue_next;
            }
            if (drive_action.sleep_session &&
                utterance.kind == VOICE_REQUEST_RECORDED_SPEECH &&
                take_state_lock()) {
                s_session_active = false;
                xSemaphoreGive(s_state_mutex);
                ESP_LOGI(TAG, "voice session closed by explicit rest command");
            }
            /* End speech at the actual audio boundary, not after a motor
             * action. This closes the mouth and starts the bounded face hold. */
            if (drive_action.approved &&
                (drive_action.explore || drive_action.draw)) {
                if (drive_action.draw) {
                    atomic_store(&s_drawing_cancel, false);
                    atomic_store(&s_drawing_active, true);
                }
                enter_action_state(drive_action.draw ? "drawing" : "exploration");
            }
            /* Keep the turn visibly owned until an optional drawing has
             * finished its full hold. IDLE here let unrelated idle work
             * treat the canvas as an interruptible standby frame. */
            if (drive_action.draw) {
                set_phase(BABY_TANK_CONVERSATION_THINKING);
            }
            if (drive_action.approved && drive_action.explore &&
                user_turn) {
                ESP_LOGI(TAG, "AI exploration group approved trace=%s",
                         drive_action.trace_id);
                execute_exploration(&drive_action);
            } else if (drive_action.approved && drive_action.draw &&
                       user_turn) {
                baby_tank_drawing_t drawing = {0};
                esp_err_t fetched = baby_tank_cloud_fetch_drawing(
                    drive_action.trace_id, &drawing);
                if (fetched == ESP_OK &&
                    (atomic_load(&s_drawing_cancel) || cancellation_requested())) {
                    fetched = ESP_ERR_INVALID_STATE;
                }
                if (fetched == ESP_OK) {
                    fetched = baby_tank_face_submit_drawing(&drawing);
                }
                if (fetched != ESP_OK && drawing.pixels != NULL) {
                    heap_caps_free(drawing.pixels);
                    drawing.pixels = NULL;
                }
                ESP_LOGI(TAG, "AI drawing trace=%s result=%s",
                         drive_action.trace_id, esp_err_to_name(fetched));
                if (fetched != ESP_OK) action_result = fetched;
                if (fetched == ESP_OK) {
                    /* Hold the scene for its full duration before opening
                     * the microphone again. No extra frame buffer is used. */
                    TickType_t started = xTaskGetTickCount();
                    while (xTaskGetTickCount() - started <
                           pdMS_TO_TICKS(drawing.duration_ms)) {
                        if (atomic_load(&s_drawing_cancel) ||
                            cancellation_requested()) {
                            baby_tank_face_cancel_drawing();
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                atomic_store(&s_drawing_active, false);
            } else if (drive_action.approved &&
                atomic_load(&sink.action_started) &&
                user_turn) {
                const char *outcome = action_result == ESP_OK ? "completed" :
                                      action_result == ESP_ERR_INVALID_STATE ? "blocked" : "failed";
                const char *reason = action_result == ESP_OK ? "stop_echo_ok" :
                                     action_result == ESP_ERR_INVALID_STATE ? "preempted_or_link_unverified" :
                                     "action_group_failed";
                ESP_LOGI(TAG, "AI drive result trace=%s result=%s reason=%s",
                         drive_action.trace_id, outcome, reason);
                esp_err_t reported = baby_tank_cloud_report_drive_action(
                    &drive_action, outcome, reason);
                if (reported != ESP_OK) {
                    ESP_LOGW(TAG, "AI drive report failed trace=%s: %s",
                             drive_action.trace_id, esp_err_to_name(reported));
                }
            }
            set_phase(BABY_TANK_CONVERSATION_IDLE);
            if (typed_turn) {
                esp_err_t reported = baby_tank_cloud_complete_text_turn(
                    utterance.text_job_id,
                    cancellation_requested() ? "blocked" :
                    action_result == ESP_OK ? "completed" : "failed");
                if (reported != ESP_OK) ESP_LOGW(TAG,
                    "typed turn completion report failed: %s",
                    esp_err_to_name(reported));
            }
            if (utterance.kind == VOICE_REQUEST_RECORDED_SPEECH &&
                take_state_lock()) {
                s_listen_deadline_us = esp_timer_get_time() +
                    (int64_t)VOICE_WAIT_FOR_SPEECH_MS * 1000LL;
                xSemaphoreGive(s_state_mutex);
            }
            complete_turn();
            if (drive_action.sleep_session) {
                (void)baby_tank_face_set_standby(true);
            }
            baby_tank_voice_runtime_log_memory();
        }
dialogue_next:
        atomic_store(&s_dialogue_busy, false);
    }
    atomic_store(&s_dialogue_busy, false);
    s_dialogue_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t baby_tank_voice_runtime_start(void)
{
    ESP_RETURN_ON_FALSE(s_afe_data == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "voice runtime already started");
    if (s_mode_mutex == NULL) {
        s_mode_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mode_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                            "mode mutex allocation failed");
    }
    atomic_store(&s_runtime_stopping, false);
    atomic_store(&s_dialogue_busy, false);
    atomic_store(&s_action_active, false);
    atomic_store(&s_drawing_active, false);
    atomic_store(&s_drawing_cancel, false);
    s_state = VOICE_STATE_IDLE;
    s_cancel_requested = false;
    s_pending_listen = false;
    s_session_active = false;
    s_touch_reaction_active = false;
    s_recorded_samples = 0U;
    s_state_mutex = xSemaphoreCreateMutex();
    s_utterance_queue = xQueueCreate(4, sizeof(voice_request_t));
    s_recording = heap_caps_malloc(
        VOICE_MAX_UTTERANCE_SAMPLES * sizeof(*s_recording),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_state_mutex != NULL && s_utterance_queue != NULL &&
                            s_recording != NULL,
                        ESP_ERR_NO_MEM, TAG,
                        "unable to allocate voice runtime buffers");

    s_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_models != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "speech model partition is unavailable");
    afe_config_t *config = afe_config_init("M", s_models, AFE_TYPE_SR,
                                           AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to allocate AFE configuration");
    ESP_RETURN_ON_FALSE(config->wakenet_model_name != NULL,
                        ESP_ERR_NOT_FOUND, TAG,
                        "WakeNet model 喵喵同学 is unavailable");
    config->aec_init = false;
    config->se_init = false;
    config->vad_init = true;
    config->wakenet_init = true;
    config->vad_min_speech_ms = 128;
    config->vad_min_noise_ms = VOICE_END_SILENCE_MS;
    config->vad_delay_ms = 160;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_perferred_core = 0;
    config->afe_perferred_priority = VOICE_FEED_TASK_PRIORITY;
    s_afe = esp_afe_handle_from_config(config);
    ESP_RETURN_ON_FALSE(s_afe != NULL, ESP_ERR_NOT_SUPPORTED, TAG,
                        "no compatible AFE implementation");
    s_afe_data = s_afe->create_from_config(config);
    afe_config_free(config);
    ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create AFE runtime");
    s_afe->print_pipeline(s_afe_data);

    BaseType_t created = xTaskCreatePinnedToCore(
        voice_feed_task, "voice_feed", VOICE_FEED_TASK_STACK, NULL,
        VOICE_FEED_TASK_PRIORITY, &s_feed_task, 0);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "unable to create voice feed task");
    created = xTaskCreatePinnedToCore(
        voice_fetch_task, "voice_fetch", VOICE_FETCH_TASK_STACK, NULL,
        VOICE_FETCH_TASK_PRIORITY, &s_fetch_task, 1);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "unable to create voice fetch task");
    created = xTaskCreatePinnedToCore(
        dialogue_task, "dialogue", VOICE_DIALOGUE_TASK_STACK, NULL,
        VOICE_DIALOGUE_TASK_PRIORITY, &s_dialogue_task, 1);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "unable to create dialogue task");
    ESP_LOGI(TAG, "WakeNet/VAD/cloud dialogue runtime ready");
    return ESP_OK;
}

bool baby_tank_voice_runtime_is_ready(void)
{
    return s_afe_data != NULL;
}

esp_err_t baby_tank_voice_runtime_stop(void)
{
    if (s_afe_data == NULL) return ESP_OK;
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    atomic_store(&s_runtime_stopping, true);
    atomic_store(&s_action_active, false);
    atomic_store(&s_drawing_cancel, true);
    baby_tank_face_cancel_drawing();
    atomic_store(&s_explore_cancel, true);
    (void)baby_tank_chassis_bridge_stop();
    baby_tank_cloud_cancel_current();
    baby_tank_audio_io_end_playback();

    if (s_state_mutex != NULL && take_state_lock()) {
        s_cancel_requested = true;
        s_pending_listen = false;
        s_session_active = false;
        s_state = VOICE_STATE_IDLE;
        reset_capture_locked();
        xSemaphoreGive(s_state_mutex);
    }
    configure_wakenet(false);
    if (s_utterance_queue != NULL) {
        voice_request_t wake = {0};
        (void)xQueueSend(s_utterance_queue, &wake, 0);
    }

    uint32_t waited_ms = 0U;
    while ((s_feed_task != NULL || s_fetch_task != NULL ||
            s_dialogue_task != NULL || atomic_load(&s_dialogue_busy)) &&
           waited_ms < VOICE_RUNTIME_STOP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20U;
    }
    if (s_feed_task != NULL) {
        ESP_LOGW(TAG, "forcing stalled voice feed task to stop");
        vTaskDelete(s_feed_task);
        s_feed_task = NULL;
    }
    if (s_fetch_task != NULL) {
        ESP_LOGW(TAG, "forcing stalled voice fetch task to stop");
        vTaskDelete(s_fetch_task);
        s_fetch_task = NULL;
    }
    if (s_dialogue_task != NULL) {
        ESP_LOGW(TAG, "forcing stalled dialogue task to stop");
        vTaskDelete(s_dialogue_task);
        s_dialogue_task = NULL;
    }
    atomic_store(&s_dialogue_busy, false);
    baby_tank_cloud_voice_session_close();

    s_afe->destroy(s_afe_data);
    s_afe_data = NULL;
    s_afe = NULL;
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
    free(s_recording);
    s_recording = NULL;
    if (s_utterance_queue != NULL) {
        vQueueDelete(s_utterance_queue);
        s_utterance_queue = NULL;
    }
    if (s_state_mutex != NULL) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "voice runtime stopped; internal SRAM released=%zu",
             after >= before ? after - before : 0U);
    return ESP_OK;
}
