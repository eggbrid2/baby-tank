#include "cloud_client.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "face_controller.h"
#include "network_runtime.h"
#include "runtime_policy.h"
#include "freertos/event_groups.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#define CLOUD_TOKEN_CAPACITY 192U
#define CLOUD_URL_CAPACITY 192U
#define CLOUD_CONFIG_RESPONSE_CAPACITY 4096U
#define CLOUD_MAX_PCM_SAMPLES (16000U * 20U)
#define CLOUD_WS_TEXT_CAPACITY 2048U
/* 250 ms of 16 kHz mono PCM. The microphone producer used to wake the
 * uploader for every AFE fragment, producing hundreds of tiny blocking TLS
 * writes per turn. A bounded 8 KiB frame keeps the upload live while
 * amortising WebSocket/TCP overhead. */
#define CLOUD_WS_PCM_FRAME_BYTES 8000U
#define CLOUD_DRAWING_JPEG_CAPACITY (96U * 1024U)
#define CLOUD_MOOD_RESPONSE_CAPACITY 2048U
#define CLOUD_STATE_RESPONSE_CAPACITY 4096U
#define CLOUD_WS_READY_BIT BIT0
#define CLOUD_WS_DONE_BIT BIT1
#define CLOUD_WS_FAILED_BIT BIT2

static const char *TAG = "baby_tank_cloud";
static char s_device_token[CLOUD_TOKEN_CAPACITY];
static bool s_initialized;
static portMUX_TYPE s_verified_identity_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_verified_device_id[65];
static atomic_bool s_cancel_requested;
static atomic_bool s_text_wait_active;
static atomic_bool s_text_wait_cancel;
static atomic_bool s_text_notice_pending;
static portMUX_TYPE s_text_notice_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_text_notice_job_id[40];
static portMUX_TYPE s_text_claim_client_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_http_client_handle_t s_text_claim_client;
static atomic_uchar s_curiosity = 78U;
static atomic_uchar s_warmth = 84U;
static atomic_uchar s_talkativeness = 46U;
static atomic_uint s_personality_revision;
static void copy_text(char *destination, size_t capacity, const char *source);
static bool valid_explore_trace(const char *trace_id);

typedef struct {
    uint8_t version;
    uint8_t needs[8];
    char mood[24];
    char band[24];
} cached_character_state_t;

static cached_character_state_t s_cached_character;
static int64_t s_last_state_cache_us;
static const char *const s_need_keys[8] = {
    "hunger", "thirst", "energy", "stamina", "joy", "excitement",
    "bond", "curiosity",
};

static void load_cached_character_state(void)
{
    nvs_handle_t handle;
    if (nvs_open("baby_state", NVS_READONLY, &handle) != ESP_OK) return;
    cached_character_state_t saved = {0};
    size_t size = sizeof(saved);
    esp_err_t result = nvs_get_blob(handle, "snapshot", &saved, &size);
    nvs_close(handle);
    if (result != ESP_OK || size != sizeof(saved) || saved.version != 1U ||
        saved.mood[sizeof(saved.mood) - 1U] != '\0' ||
        saved.band[sizeof(saved.band) - 1U] != '\0') return;
    s_cached_character = saved;
    (void)baby_tank_face_set_mood(saved.mood, 55U);
    (void)baby_tank_face_set_time_band(saved.band);
    ESP_LOGI(TAG, "restored offline character snapshot; cloud refresh pending");
}

static void cache_character_state(const cJSON *state, const char *mood,
                                  const char *band)
{
    const cJSON *needs = cJSON_GetObjectItemCaseSensitive(state, "needs");
    if (!cJSON_IsObject(needs) || mood == NULL || band == NULL ||
        strlen(mood) >= sizeof(s_cached_character.mood) ||
        strlen(band) >= sizeof(s_cached_character.band)) return;
    cached_character_state_t next = { .version = 1U };
    for (size_t index = 0; index < 8U; ++index) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(needs,
                                                               s_need_keys[index]);
        if (!cJSON_IsNumber(value) || value->valueint < 0 ||
            value->valueint > 100) return;
        next.needs[index] = (uint8_t)value->valueint;
    }
    copy_text(next.mood, sizeof(next.mood), mood);
    copy_text(next.band, sizeof(next.band), band);
    if (memcmp(&next, &s_cached_character, sizeof(next)) == 0) return;
    int64_t now = esp_timer_get_time();
    bool band_changed = strcmp(next.band, s_cached_character.band) != 0;
    if (!band_changed && s_last_state_cache_us != 0 &&
        now - s_last_state_cache_us < 30LL * 60LL * 1000000LL) return;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("baby_state", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "snapshot", &next, sizeof(next));
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result == ESP_OK) {
        s_cached_character = next;
        s_last_state_cache_us = now;
    } else {
        ESP_LOGW(TAG, "offline character snapshot was not saved: %s",
                 esp_err_to_name(result));
    }
}

uint32_t baby_tank_cloud_personality_revision(void)
{
    return atomic_load(&s_personality_revision);
}

baby_tank_personality_t baby_tank_cloud_get_personality(void)
{
    return (baby_tank_personality_t) {
        .curiosity = atomic_load(&s_curiosity),
        .warmth = atomic_load(&s_warmth),
        .talkativeness = atomic_load(&s_talkativeness),
    };
}

esp_err_t baby_tank_cloud_set_personality(baby_tank_personality_t personality)
{
    ESP_RETURN_ON_FALSE(personality.curiosity <= 100U &&
                            personality.warmth <= 100U &&
                            personality.talkativeness <= 100U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid personality trait");
    baby_tank_personality_t current = baby_tank_cloud_get_personality();
    if (current.curiosity == personality.curiosity &&
        current.warmth == personality.warmth &&
        current.talkativeness == personality.talkativeness) return ESP_OK;
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("baby_persona", NVS_READWRITE, &handle),
                        TAG, "unable to open personality storage");
    const uint8_t saved[4] = {1U, personality.curiosity,
                              personality.warmth, personality.talkativeness};
    esp_err_t result = nvs_set_blob(handle, "traits", saved, sizeof(saved));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    atomic_store(&s_curiosity, personality.curiosity);
    atomic_store(&s_warmth, personality.warmth);
    atomic_store(&s_talkativeness, personality.talkativeness);
    ESP_LOGI(TAG, "personality settings updated locally");
    return ESP_OK;
}

static void load_personality(void)
{
    nvs_handle_t handle;
    if (nvs_open("baby_persona", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t saved[4] = {0};
    size_t size = sizeof(saved);
    esp_err_t result = nvs_get_blob(handle, "traits", saved, &size);
    nvs_close(handle);
    if (result != ESP_OK || size != sizeof(saved) || saved[0] != 1U ||
        saved[1] > 100U || saved[2] > 100U || saved[3] > 100U) return;
    atomic_store(&s_curiosity, saved[1]);
    atomic_store(&s_warmth, saved[2]);
    atomic_store(&s_talkativeness, saved[3]);
    nvs_handle_t revision_handle;
    if (nvs_open("baby_persona", NVS_READONLY, &revision_handle) == ESP_OK) {
        uint32_t revision = 0U;
        if (nvs_get_u32(revision_handle, "revision", &revision) == ESP_OK) {
            atomic_store(&s_personality_revision, revision);
        }
        nvs_close(revision_handle);
    }
}

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
    baby_tank_cloud_audio_chunk_cb_t audio_callback;
    baby_tank_cloud_reply_meta_cb_t metadata_callback;
    baby_tank_cloud_reply_cue_cb_t cue_callback;
    void *audio_context;
    baby_tank_cloud_audio_format_t audio_format;
    uint32_t audio_sample_rate;
    esp_err_t callback_result;
    char emotion[24];
    uint8_t intensity;
    char face_cues[128];
    char mood[24];
    uint8_t mood_intensity;
    char thought[16];
    char thought_display_b64[32];
    char session_mode[12];
    bool metadata_delivered;
    char action_version[4];
    char action_mode[12];
    char action_status[12];
    char action_type[12];
    char action_direction[12];
    char action_speed[8];
    char action_duration[8];
    char action_light_mode[12];
    char trace_id[40];
    char content_type[32];
    char drawing_duration[8];
    bool framed_audio;
    uint8_t frame_header[5];
    uint8_t frame_header_used;
    uint8_t frame_type;
    uint32_t frame_remaining;
    size_t framed_pcm_bytes;
    char frame_cue[40];
    uint8_t frame_cue_used;
} cloud_response_t;

static esp_websocket_client_handle_t s_voice_socket;
static EventGroupHandle_t s_voice_socket_events;
static cloud_response_t *s_voice_socket_response;
static baby_tank_cloud_drive_action_t *s_voice_socket_action;
static esp_err_t s_voice_socket_result;
static int s_voice_socket_status;
static char s_voice_socket_text[CLOUD_WS_TEXT_CAPACITY];

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0U) return;
    snprintf(destination, capacity, "%s", source != NULL ? source : "");
}

static esp_http_client_handle_t take_text_claim_client(void)
{
    taskENTER_CRITICAL(&s_text_claim_client_lock);
    esp_http_client_handle_t client = s_text_claim_client;
    s_text_claim_client = NULL;
    taskEXIT_CRITICAL(&s_text_claim_client_lock);
    return client;
}

static void save_text_claim_client(esp_http_client_handle_t client)
{
    taskENTER_CRITICAL(&s_text_claim_client_lock);
    esp_http_client_handle_t previous = s_text_claim_client;
    s_text_claim_client = client;
    taskEXIT_CRITICAL(&s_text_claim_client_lock);
    if (previous != NULL) esp_http_client_cleanup(previous);
}

static void discard_text_claim_client(void)
{
    esp_http_client_handle_t client = take_text_claim_client();
    if (client != NULL) esp_http_client_cleanup(client);
}

static bool header_equals(const char *header, const char *expected)
{
    return header != NULL && strcasecmp(header, expected) == 0;
}

static void copy_header_value(char *destination, size_t capacity,
                              const char *value)
{
    copy_text(destination, capacity, value);
}

static bool decode_base64url_text(const char *encoded,
                                  char *decoded, size_t capacity)
{
    if (encoded == NULL || decoded == NULL || capacity < 2U) return false;
    size_t length = strnlen(encoded, 25U);
    if (length == 0U || length >= 25U) return false;
    char padded[28];
    size_t padded_length = length;
    for (size_t index = 0U; index < length; ++index) {
        char value = encoded[index];
        padded[index] = value == '-' ? '+' : value == '_' ? '/' : value;
    }
    while ((padded_length & 3U) != 0U) padded[padded_length++] = '=';
    size_t output_length = 0U;
    int result = mbedtls_base64_decode(
        (unsigned char *)decoded, capacity - 1U, &output_length,
        (const unsigned char *)padded, padded_length);
    if (result != 0 || output_length == 0U || output_length >= capacity) {
        decoded[0] = '\0';
        return false;
    }
    decoded[output_length] = '\0';
    return true;
}

static baby_tank_cloud_audio_format_t audio_format_from_string(const char *value)
{
    if (value == NULL) return BABY_TANK_CLOUD_AUDIO_UNKNOWN;
    if (strcasecmp(value, "pcm") == 0 ||
        strncasecmp(value, "audio/pcm", 9U) == 0) {
        return BABY_TANK_CLOUD_AUDIO_PCM_S16LE;
    }
    if (strcasecmp(value, "mp3") == 0 ||
        strncasecmp(value, "audio/mpeg", 10U) == 0) {
        return BABY_TANK_CLOUD_AUDIO_MP3;
    }
    if (strcasecmp(value, "ogg_opus") == 0 ||
        strncasecmp(value, "audio/ogg", 9U) == 0) {
        return BABY_TANK_CLOUD_AUDIO_OGG_OPUS;
    }
    return BABY_TANK_CLOUD_AUDIO_UNKNOWN;
}

static esp_err_t deliver_audio(cloud_response_t *response,
                               const uint8_t *data, size_t size)
{
    if (response->metadata_callback != NULL && !response->metadata_delivered) {
        char thought_text[16] = {0};
        if (response->thought_display_b64[0] != '\0' &&
            !decode_base64url_text(response->thought_display_b64,
                                   thought_text, sizeof(thought_text))) {
            ESP_LOGW(TAG, "invalid cloud dream caption encoding");
        }
        response->metadata_delivered = true;
        response->callback_result = response->metadata_callback(
            response->emotion[0] != '\0' ? response->emotion : "calm",
            response->intensity != 0U ? response->intensity : 55U,
            response->face_cues,
            response->mood[0] != '\0' ? response->mood : "calm",
            response->mood_intensity != 0U ? response->mood_intensity : 45U,
            response->thought[0] != '\0' ? response->thought : "cloud",
            thought_text,
            response->audio_context);
        if (response->callback_result != ESP_OK) return response->callback_result;
    }
    response->callback_result = response->audio_callback(
        data, size, response->audio_format, response->audio_sample_rate,
        response->audio_context);
    return response->callback_result;
}

static esp_err_t deliver_face_cue(cloud_response_t *response)
{
    if (response->cue_callback == NULL || response->framed_pcm_bytes % 2U != 0U ||
        response->framed_pcm_bytes == 0U ||
        response->frame_cue_used == 0U ||
        response->frame_cue_used >= sizeof(response->frame_cue)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    response->frame_cue[response->frame_cue_used] = '\0';
    if (strlen(response->frame_cue) != response->frame_cue_used) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    char *colon = strchr(response->frame_cue, ':');
    if (colon == NULL || colon == response->frame_cue) return ESP_ERR_INVALID_RESPONSE;
    *colon = '\0';
    char *end = NULL;
    unsigned long intensity = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || intensity < 20UL ||
        intensity > 100UL) return ESP_ERR_INVALID_RESPONSE;
    response->callback_result = response->cue_callback(
        response->framed_pcm_bytes / 2U, response->frame_cue,
        (uint8_t)intensity, response->audio_context);
    return response->callback_result;
}

static esp_err_t parse_framed_audio(cloud_response_t *response,
                                    const uint8_t *data, size_t size)
{
    size_t offset = 0U;
    while (offset < size) {
        if (response->frame_header_used < sizeof(response->frame_header)) {
            size_t needed = sizeof(response->frame_header) -
                            response->frame_header_used;
            size_t count = size - offset < needed ? size - offset : needed;
            memcpy(response->frame_header + response->frame_header_used,
                   data + offset, count);
            response->frame_header_used += (uint8_t)count;
            offset += count;
            if (response->frame_header_used < sizeof(response->frame_header)) break;
            response->frame_type = response->frame_header[0];
            response->frame_remaining =
                ((uint32_t)response->frame_header[1] << 24U) |
                ((uint32_t)response->frame_header[2] << 16U) |
                ((uint32_t)response->frame_header[3] << 8U) |
                (uint32_t)response->frame_header[4];
            if ((response->frame_type == 'P' &&
                 (response->frame_remaining == 0U ||
                  response->frame_remaining > 4096U)) ||
                (response->frame_type == 'E' &&
                 (response->frame_remaining == 0U ||
                  response->frame_remaining >= sizeof(response->frame_cue))) ||
                (response->frame_type != 'P' && response->frame_type != 'E')) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            response->frame_cue_used = 0U;
            if (offset == size) break;
        }
        size_t count = size - offset;
        if (count > response->frame_remaining) count = response->frame_remaining;
        if (response->frame_type == 'P') {
            if (response->framed_pcm_bytes + count > 2U * 1024U * 1024U) {
                return ESP_ERR_INVALID_SIZE;
            }
            esp_err_t result = deliver_audio(response, data + offset, count);
            if (result != ESP_OK) return result;
            response->framed_pcm_bytes += count;
        } else {
            memcpy(response->frame_cue + response->frame_cue_used,
                   data + offset, count);
            response->frame_cue_used += (uint8_t)count;
        }
        offset += count;
        response->frame_remaining -= (uint32_t)count;
        if (response->frame_remaining == 0U) {
            if (response->frame_type == 'E') {
                esp_err_t result = deliver_face_cue(response);
                if (result != ESP_OK) return result;
            }
            response->frame_header_used = 0U;
        }
    }
    return ESP_OK;
}

static esp_err_t cloud_http_event(esp_http_client_event_t *event)
{
    if (atomic_load_explicit(&s_cancel_requested, memory_order_relaxed)) {
        return ESP_ERR_INVALID_STATE;
    }
    cloud_response_t *response = event->user_data;
    if (response == NULL) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (header_equals(event->header_key, "x-audio-format")) {
            char value[24];
            copy_header_value(value, sizeof(value), event->header_value);
            response->audio_format = audio_format_from_string(value);
        } else if (header_equals(event->header_key,
                                 "x-audio-sample-rate")) {
            char value[16];
            copy_header_value(value, sizeof(value), event->header_value);
            response->audio_sample_rate = (uint32_t)strtoul(value, NULL, 10);
        } else if (header_equals(event->header_key, "content-type")) {
            char value[32];
            copy_header_value(value, sizeof(value), event->header_value);
            copy_header_value(response->content_type,
                              sizeof(response->content_type), value);
            if (response->audio_format == BABY_TANK_CLOUD_AUDIO_UNKNOWN) {
                response->audio_format = audio_format_from_string(value);
            }
        } else if (header_equals(event->header_key, "x-ai-emotion")) {
            copy_header_value(response->emotion, sizeof(response->emotion),
                              event->header_value);
        } else if (header_equals(event->header_key, "x-ai-intensity")) {
            response->intensity = (uint8_t)strtoul(event->header_value,
                                                    NULL, 10);
        } else if (header_equals(event->header_key, "x-ai-face-cues")) {
            copy_header_value(response->face_cues, sizeof(response->face_cues),
                              event->header_value);
        } else if (header_equals(event->header_key, "x-ai-mood")) {
            copy_header_value(response->mood, sizeof(response->mood),
                              event->header_value);
        } else if (header_equals(event->header_key, "x-ai-mood-intensity")) {
            response->mood_intensity = (uint8_t)strtoul(event->header_value,
                                                         NULL, 10);
        } else if (header_equals(event->header_key, "x-ai-thought")) {
            copy_header_value(response->thought, sizeof(response->thought),
                              event->header_value);
        } else if (header_equals(event->header_key,
                                 "x-ai-thought-display-b64")) {
            copy_header_value(response->thought_display_b64,
                              sizeof(response->thought_display_b64),
                              event->header_value);
        } else if (header_equals(event->header_key, "x-ai-session")) {
            copy_header_value(response->session_mode,
                              sizeof(response->session_mode),
                              event->header_value);
        } else if (header_equals(event->header_key, "x-ai-action-version")) {
            copy_header_value(response->action_version, sizeof(response->action_version), event->header_value);
        } else if (header_equals(event->header_key, "x-ai-action-mode")) {
            copy_header_value(response->action_mode, sizeof(response->action_mode), event->header_value);
        } else if (header_equals(event->header_key, "x-ai-action-status")) {
            copy_header_value(response->action_status, sizeof(response->action_status), event->header_value);
        } else if (header_equals(event->header_key, "x-ai-action-type")) {
            copy_header_value(response->action_type, sizeof(response->action_type), event->header_value);
        } else if (header_equals(event->header_key, "x-ai-action-direction")) {
            copy_header_value(response->action_direction, sizeof(response->action_direction), event->header_value);
        } else if (header_equals(event->header_key, "x-ai-action-speed-per-mille")) {
            copy_header_value(response->action_speed, sizeof(response->action_speed), event->header_value);
        } else if (header_equals(event->header_key, "x-ai-action-duration-ms")) {
            copy_header_value(response->action_duration, sizeof(response->action_duration), event->header_value);
        } else if (header_equals(event->header_key, "x-ai-action-light-mode")) {
            copy_header_value(response->action_light_mode,
                              sizeof(response->action_light_mode),
                              event->header_value);
        } else if (header_equals(event->header_key, "x-turn-trace-id")) {
            copy_header_value(response->trace_id, sizeof(response->trace_id), event->header_value);
        } else if (header_equals(event->header_key,
                                 "x-drawing-duration-ms")) {
            copy_header_value(response->drawing_duration,
                              sizeof(response->drawing_duration),
                              event->header_value);
        } else if (header_equals(event->header_key,
                                 "x-babytank-audio-framing")) {
            response->framed_audio = strcmp(event->header_value, "v1") == 0;
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        int status = esp_http_client_get_status_code(event->client);
        if (response->audio_callback != NULL && status == 200) {
            response->callback_result = response->framed_audio
                ? parse_framed_audio(response, event->data,
                                     (size_t)event->data_len)
                : deliver_audio(response, event->data,
                                (size_t)event->data_len);
            return response->callback_result;
        }
        if (response->buffer != NULL) {
            size_t remaining = response->capacity > response->length
                                   ? response->capacity - response->length - 1U
                                   : 0U;
            size_t copy_length = (size_t)event->data_len;
            if (copy_length > remaining) {
                copy_length = remaining;
                response->overflow = true;
            }
            if (copy_length > 0U) {
                memcpy(response->buffer + response->length,
                       event->data, copy_length);
                response->length += copy_length;
                response->buffer[response->length] = '\0';
            }
        }
    }
    return ESP_OK;
}

static uint8_t action_light_mode_from_string(const char *value)
{
    if (value == NULL || strcmp(value, "unchanged") == 0) return 0U;
    if (strcmp(value, "white") == 0) return 2U;
    if (strcmp(value, "rainbow") == 0) return 6U;
    if (strcmp(value, "sleep") == 0) return 8U;
    return UINT8_MAX;
}

static void parse_stream_action(const cloud_response_t *response,
                                baby_tank_cloud_drive_action_t *action)
{
    if (response == NULL || action == NULL) return;
    action->sleep_session = strcmp(response->session_mode, "sleep") == 0;
    if ((strcmp(response->action_version, "4") == 0 ||
         strcmp(response->action_version, "2") == 0) &&
        strcmp(response->action_mode, "execute") == 0 &&
        strcmp(response->action_status, "approved") == 0 &&
        strcmp(response->action_type, "drive") == 0 &&
        strlen(response->trace_id) == 36U &&
        (strcmp(response->action_direction, "forward") == 0 ||
         strcmp(response->action_direction, "backward") == 0 ||
         strcmp(response->action_direction, "left") == 0 ||
         strcmp(response->action_direction, "right") == 0)) {
        char *speed_end = NULL;
        char *duration_end = NULL;
        unsigned long speed = strtoul(response->action_speed, &speed_end, 10);
        unsigned long duration = strtoul(response->action_duration,
                                         &duration_end, 10);
        uint8_t light_mode = action_light_mode_from_string(
            response->action_light_mode);
        if (response->action_speed[0] != '\0' && *speed_end == '\0' &&
            speed >= 700UL && speed <= 1000UL &&
            response->action_duration[0] != '\0' &&
            *duration_end == '\0' && duration >= 2000UL &&
            duration <= 9000UL && light_mode != UINT8_MAX) {
            action->approved = true;
            copy_text(action->trace_id, sizeof(action->trace_id),
                      response->trace_id);
            copy_text(action->direction, sizeof(action->direction),
                      response->action_direction);
            action->speed_per_mille = (uint16_t)speed;
            action->duration_ms = (uint16_t)duration;
            action->light_mode = light_mode;
        }
    }
    if (strcmp(response->action_version, "3") == 0 &&
        strcmp(response->action_mode, "execute") == 0 &&
        strcmp(response->action_status, "approved") == 0 &&
        strcmp(response->action_type, "explore") == 0 &&
        strlen(response->trace_id) == 36U) {
        action->approved = true;
        action->explore = true;
        copy_text(action->trace_id, sizeof(action->trace_id),
                  response->trace_id);
    }
    if (strcmp(response->action_version, "5") == 0 &&
        strcmp(response->action_mode, "execute") == 0 &&
        strcmp(response->action_status, "approved") == 0 &&
        strcmp(response->action_type, "look") == 0 &&
        strlen(response->trace_id) == 36U) {
        action->approved = true;
        action->look = true;
        copy_text(action->trace_id, sizeof(action->trace_id),
                  response->trace_id);
    }
    if (strcmp(response->action_version, "6") == 0 &&
        strcmp(response->action_mode, "execute") == 0 &&
        strcmp(response->action_status, "approved") == 0 &&
        strcmp(response->action_type, "draw") == 0 &&
        strlen(response->trace_id) == 36U) {
        action->approved = true;
        action->draw = true;
        copy_text(action->trace_id, sizeof(action->trace_id),
                  response->trace_id);
    }
}

static esp_err_t perform_request(const char *path,
                                 esp_http_client_method_t method,
                                 const char *content_type,
                                 const void *body,
                                 size_t body_size,
                                 cloud_response_t *response,
                                 int *status_code, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "cloud client is not initialized");
    char url[CLOUD_URL_CAPACITY];
    int url_length = snprintf(url, sizeof(url), "%s%s",
                              strncmp(path, "/api/", 5U) == 0
                                  ? BABY_TANK_CLOUD_ORIGIN
                                  : BABY_TANK_CLOUD_BASE_URL,
                              path);
    ESP_RETURN_ON_FALSE(url_length > 0 && url_length < (int)sizeof(url),
                        ESP_ERR_INVALID_SIZE, TAG, "cloud URL is too long");
    char authorization[CLOUD_TOKEN_CAPACITY + 16U];
    snprintf(authorization, sizeof(authorization), "Bearer %s", s_device_token);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = cloud_http_event,
        .user_data = response,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create HTTP client");
    esp_err_t result = esp_http_client_set_method(client, method);
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (result == ESP_OK && content_type != NULL) {
        result = esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (result == ESP_OK && body != NULL) {
        if (body_size > INT32_MAX) {
            ESP_LOGE(TAG, "request body is too large");
            result = ESP_ERR_INVALID_SIZE;
        } else {
            result = esp_http_client_set_post_field(client, body,
                                                    (int)body_size);
        }
    }
    if (result == ESP_OK) result = esp_http_client_perform(client);
    if (status_code != NULL) *status_code = esp_http_client_get_status_code(client);
    if (result == ESP_OK && response != NULL && response->callback_result != ESP_OK) {
        result = response->callback_result;
    }
    memset(authorization, 0, sizeof(authorization));
    esp_http_client_cleanup(client);
    return result;
}

static esp_err_t perform_reused_text_request(
    esp_http_client_handle_t client, const char *path,
    cloud_response_t *response, int *status_code, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(client != NULL && path != NULL && response != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid reusable text connection");
    char url[CLOUD_URL_CAPACITY];
    int url_length = snprintf(url, sizeof(url), "%s%s",
                              BABY_TANK_CLOUD_BASE_URL, path);
    ESP_RETURN_ON_FALSE(url_length > 0 && url_length < (int)sizeof(url),
                        ESP_ERR_INVALID_SIZE, TAG, "text URL is too long");
    esp_err_t result = esp_http_client_set_url(client, url);
    if (result == ESP_OK) {
        result = esp_http_client_set_method(client, HTTP_METHOD_POST);
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_post_field(client, NULL, 0);
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_user_data(client, response);
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_timeout_ms(client, timeout_ms);
    }
    int64_t started_us = esp_timer_get_time();
    while (result == ESP_OK) {
        result = esp_http_client_perform(client);
        if (result != ESP_ERR_HTTP_EAGAIN) break;
        if (atomic_load(&s_cancel_requested)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (esp_timer_get_time() - started_us >
            (int64_t)timeout_ms * 1000LL) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        result = ESP_OK;
    }
    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    if (result == ESP_OK && response->callback_result != ESP_OK) {
        result = response->callback_result;
    }
    (void)esp_http_client_set_user_data(client, NULL);
    return result;
}

static esp_err_t require_success(int status_code)
{
    if (status_code == 200) return ESP_OK;
    if (status_code == 401 || status_code == 403) return ESP_ERR_INVALID_STATE;
    if (status_code == 404 || status_code == 501) return ESP_ERR_NOT_SUPPORTED;
    if (status_code == 413) return ESP_ERR_INVALID_SIZE;
    if (status_code == 429) return ESP_ERR_TIMEOUT;
    return ESP_FAIL;
}

static void parse_service(cJSON *services, const char *name,
                          baby_tank_cloud_service_t *service)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(services, name);
    if (!cJSON_IsObject(item)) return;
    cJSON *provider = cJSON_GetObjectItemCaseSensitive(item, "provider");
    cJSON *model = cJSON_GetObjectItemCaseSensitive(item, "model");
    if (!cJSON_IsString(provider) || !cJSON_IsString(model)) return;
    service->available = true;
    copy_text(service->provider, sizeof(service->provider), provider->valuestring);
    copy_text(service->model, sizeof(service->model), model->valuestring);
}

esp_err_t baby_tank_cloud_init(const char *device_token)
{
    ESP_RETURN_ON_FALSE(device_token != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "device token is required");
    size_t length = strnlen(device_token, sizeof(s_device_token));
    ESP_RETURN_ON_FALSE(length >= 24U && length < sizeof(s_device_token),
                        ESP_ERR_INVALID_ARG, TAG, "invalid device token length");
    memset(s_device_token, 0, sizeof(s_device_token));
    memcpy(s_device_token, device_token, length);
    taskENTER_CRITICAL(&s_verified_identity_lock);
    memset(s_verified_device_id, 0, sizeof(s_verified_device_id));
    taskEXIT_CRITICAL(&s_verified_identity_lock);
    load_personality();
    load_cached_character_state();
    s_initialized = true;
    ESP_LOGI(TAG, "Baby Tank Cloud client ready; verified identity pending");
    return ESP_OK;
}

esp_err_t baby_tank_cloud_copy_verified_device_id(char *output, size_t capacity)
{
    ESP_RETURN_ON_FALSE(output != NULL && capacity >= sizeof(s_verified_device_id),
                        ESP_ERR_INVALID_ARG, TAG, "identity output is too small");
    taskENTER_CRITICAL(&s_verified_identity_lock);
    memcpy(output, s_verified_device_id, sizeof(s_verified_device_id));
    taskEXIT_CRITICAL(&s_verified_identity_lock);
    return output[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t baby_tank_cloud_fetch_verified_identity(void)
{
    char body[192] = {0};
    cloud_response_t response = {
        .buffer = body,
        .capacity = sizeof(body),
        .callback_result = ESP_OK,
    };
    int status = 0;
    ESP_RETURN_ON_ERROR(perform_request("/identity", HTTP_METHOD_GET, NULL,
                                        NULL, 0U, &response, &status, 8000),
                        TAG, "device identity request failed");
    ESP_RETURN_ON_ERROR(require_success(status), TAG,
                        "device identity rejected");
    ESP_RETURN_ON_FALSE(!response.overflow, ESP_ERR_INVALID_SIZE, TAG,
                        "device identity response is too large");
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid device identity JSON");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
    const char *value = cJSON_IsString(id) ? id->valuestring : NULL;
    size_t length = value != NULL ? strnlen(value, sizeof(s_verified_device_id)) : 0U;
    bool valid = length > 0U && length < sizeof(s_verified_device_id) &&
        strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                       "0123456789._-") == length;
    if (valid) {
        taskENTER_CRITICAL(&s_verified_identity_lock);
        memset(s_verified_device_id, 0, sizeof(s_verified_device_id));
        memcpy(s_verified_device_id, value, length);
        taskEXIT_CRITICAL(&s_verified_identity_lock);
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t baby_tank_cloud_fetch_config(
    baby_tank_cloud_runtime_config_t *runtime_config)
{
    ESP_RETURN_ON_FALSE(runtime_config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "runtime config is required");
    char body[CLOUD_CONFIG_RESPONSE_CAPACITY] = {0};
    cloud_response_t response = {
        .buffer = body,
        .capacity = sizeof(body),
        .callback_result = ESP_OK,
    };
    int status = 0;
    ESP_RETURN_ON_ERROR(perform_request("/config", HTTP_METHOD_GET, NULL,
                                        NULL, 0U, &response, &status, 120000),
                        TAG, "cloud config request failed");
    ESP_RETURN_ON_ERROR(require_success(status), TAG,
                        "cloud config rejected");
    ESP_RETURN_ON_FALSE(!response.overflow, ESP_ERR_INVALID_SIZE, TAG,
                        "cloud config response is too large");

    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid cloud config JSON");
    memset(runtime_config, 0, sizeof(*runtime_config));
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
    cJSON *services = cJSON_GetObjectItemCaseSensitive(root, "services");
    if (cJSON_IsString(revision)) {
        copy_text(runtime_config->revision,
                  sizeof(runtime_config->revision), revision->valuestring);
    }
    if (cJSON_IsObject(services)) {
        parse_service(services, "llm", &runtime_config->llm);
        parse_service(services, "asr", &runtime_config->asr);
        parse_service(services, "tts", &runtime_config->tts);
        cJSON *tts = cJSON_GetObjectItemCaseSensitive(services, "tts");
        cJSON *options = cJSON_IsObject(tts)
                             ? cJSON_GetObjectItemCaseSensitive(tts, "options")
                             : NULL;
        cJSON *format = cJSON_IsObject(options)
                            ? cJSON_GetObjectItemCaseSensitive(options, "audioFormat")
                            : NULL;
        cJSON *sample_rate = cJSON_IsObject(options)
                                 ? cJSON_GetObjectItemCaseSensitive(options, "sampleRate")
                                 : NULL;
        if (cJSON_IsString(format)) {
            runtime_config->tts_format =
                audio_format_from_string(format->valuestring);
        }
        if (cJSON_IsString(sample_rate)) {
            runtime_config->tts_sample_rate =
                (uint32_t)strtoul(sample_rate->valuestring, NULL, 10);
        } else if (cJSON_IsNumber(sample_rate)) {
            runtime_config->tts_sample_rate = (uint32_t)sample_rate->valuedouble;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "cloud config llm=%s asr=%s tts=%s",
             runtime_config->llm.available ? runtime_config->llm.model : "missing",
             runtime_config->asr.available ? runtime_config->asr.model : "missing",
             runtime_config->tts.available ? runtime_config->tts.model : "missing");
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) cache_sleep_thought(
    const char *key, const char *text)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("baby_dream", NVS_READWRITE, &handle),
                        TAG, "unable to open dream cache");
    esp_err_t result = nvs_set_str(handle, "key", key);
    if (result == ESP_OK) result = nvs_set_str(handle, "text", text);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

typedef struct {
    uint8_t version;
    uint8_t count;
    baby_tank_dream_entry_t entries[BABY_TANK_DREAM_DECK_CAPACITY];
} cached_dream_deck_t;

static esp_err_t cache_dream_deck(const baby_tank_dream_entry_t *entries,
                                  size_t count)
{
    ESP_RETURN_ON_FALSE(entries != NULL && count == BABY_TANK_DREAM_DECK_CAPACITY,
                        ESP_ERR_INVALID_ARG, TAG, "dream deck must have 30 cards");
    cached_dream_deck_t saved = {.version = 1U, .count = (uint8_t)count};
    memcpy(saved.entries, entries, count * sizeof(entries[0]));
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("baby_dream", NVS_READWRITE, &handle),
                        TAG, "unable to open dream deck cache");
    esp_err_t result = nvs_set_blob(handle, "deck", &saved, sizeof(saved));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t baby_tank_cloud_restore_sleep_thought(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("baby_dream", NVS_READONLY, &handle);
    if (result != ESP_OK) return result;
    cached_dream_deck_t deck = {0};
    size_t deck_size = sizeof(deck);
    result = nvs_get_blob(handle, "deck", &deck, &deck_size);
    if (result == ESP_OK && deck_size == sizeof(deck) && deck.version == 1U &&
        deck.count == BABY_TANK_DREAM_DECK_CAPACITY) {
        nvs_close(handle);
        ESP_RETURN_ON_ERROR(baby_tank_display_set_dream_deck(
                                deck.entries, deck.count),
                            TAG, "cached dream deck is invalid");
        ESP_LOGI(TAG, "restored cached dream deck cards=%u",
                 (unsigned)deck.count);
        return ESP_OK;
    }
    char key[16] = {0};
    char text[16] = {0};
    size_t key_size = sizeof(key);
    size_t text_size = sizeof(text);
    result = nvs_get_str(handle, "key", key, &key_size);
    if (result == ESP_OK) result = nvs_get_str(handle, "text", text, &text_size);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    ESP_RETURN_ON_ERROR(baby_tank_face_set_thought(key), TAG,
                        "cached dream key is invalid");
    ESP_RETURN_ON_ERROR(baby_tank_face_set_thought_text(text), TAG,
                        "cached dream caption is invalid");
    ESP_LOGI(TAG, "restored cached dream caption");
    return ESP_OK;
}

esp_err_t baby_tank_cloud_refresh_sleep_thought(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "cloud client is not initialized");
    char body[CLOUD_MOOD_RESPONSE_CAPACITY] = {0};
    cloud_response_t response = {
        .buffer = body,
        .capacity = sizeof(body),
        .callback_result = ESP_OK,
    };
    int status = 0;
    ESP_RETURN_ON_ERROR(perform_request("/dream-deck", HTTP_METHOD_GET, NULL,
                                        NULL, 0U, &response, &status, 25000),
                        TAG, "dream deck refresh failed");
    ESP_RETURN_ON_ERROR(require_success(status), TAG,
                        "dream deck refresh rejected");
    ESP_RETURN_ON_FALSE(!response.overflow, ESP_ERR_INVALID_SIZE, TAG,
                        "dream deck response is too large");
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid dream deck JSON");
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(entries) ||
        cJSON_GetArraySize(entries) != BABY_TANK_DREAM_DECK_CAPACITY) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    baby_tank_dream_entry_t parsed[BABY_TANK_DREAM_DECK_CAPACITY] = {0};
    for (size_t index = 0U; index < BABY_TANK_DREAM_DECK_CAPACITY; ++index) {
        const cJSON *item = cJSON_GetArrayItem(entries, (int)index);
        const cJSON *key = cJSON_IsArray(item) ? cJSON_GetArrayItem(item, 0) : NULL;
        const cJSON *text = cJSON_IsArray(item) ? cJSON_GetArrayItem(item, 1) : NULL;
        if (!cJSON_IsString(key) || !cJSON_IsString(text) ||
            baby_tank_display_make_dream_entry(key->valuestring,
                                               text->valuestring,
                                               &parsed[index]) != ESP_OK) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    cJSON_Delete(root);
    ESP_RETURN_ON_ERROR(baby_tank_display_set_dream_deck(
                            parsed, BABY_TANK_DREAM_DECK_CAPACITY),
                        TAG, "cloud dream deck is invalid");
    ESP_RETURN_ON_ERROR(cache_dream_deck(
                            parsed, BABY_TANK_DREAM_DECK_CAPACITY),
                        TAG, "unable to cache dream deck");
    ESP_LOGI(TAG, "sleep dream deck refreshed cards=%u",
             (unsigned)BABY_TANK_DREAM_DECK_CAPACITY);
    return ESP_OK;
}

esp_err_t baby_tank_cloud_refresh_character_mood(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "cloud client is not initialized");
    if (atomic_load(&s_personality_revision) == 0U) {
        baby_tank_personality_t current = baby_tank_cloud_get_personality();
        char bootstrap[128];
        int length = snprintf(bootstrap, sizeof(bootstrap),
            "{\"curiosity\":%u,\"warmth\":%u,\"talkativeness\":%u}",
            (unsigned)current.curiosity, (unsigned)current.warmth,
            (unsigned)current.talkativeness);
        ESP_RETURN_ON_FALSE(length > 0 && length < (int)sizeof(bootstrap),
                            ESP_ERR_INVALID_SIZE, TAG,
                            "personality bootstrap body too large");
        char bootstrap_response[256] = {0};
        cloud_response_t bootstrap_result = { .buffer = bootstrap_response,
            .capacity = sizeof(bootstrap_response), .callback_result = ESP_OK };
        int bootstrap_status = 0;
        ESP_RETURN_ON_ERROR(perform_request(
            "/api/device/v2/personality/bootstrap", HTTP_METHOD_POST,
            "application/json", bootstrap, (size_t)length,
            &bootstrap_result, &bootstrap_status, 8000), TAG,
            "personality bootstrap failed");
        ESP_RETURN_ON_ERROR(require_success(bootstrap_status), TAG,
            "personality bootstrap rejected");
    }
    char *body = heap_caps_calloc(1U, CLOUD_STATE_RESPONSE_CAPACITY,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG,
                        "character state response allocation failed");
    cloud_response_t response = {
        .buffer = body,
        .capacity = CLOUD_STATE_RESPONSE_CAPACITY,
        .callback_result = ESP_OK,
    };
    int status = 0;
    esp_err_t fetch_result = perform_request("/api/device/v2/state",
        HTTP_METHOD_GET, NULL, NULL, 0U, &response, &status, 8000);
    if (fetch_result == ESP_OK) fetch_result = require_success(status);
    if (fetch_result == ESP_OK && response.overflow) {
        fetch_result = ESP_ERR_INVALID_SIZE;
    }
    if (fetch_result != ESP_OK) {
        heap_caps_free(body);
        return fetch_result;
    }
    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid character state JSON");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *clock = cJSON_GetObjectItemCaseSensitive(root, "clock");
    const cJSON *policy = cJSON_GetObjectItemCaseSensitive(state, "policy");
    const cJSON *personality = cJSON_GetObjectItemCaseSensitive(root, "personality");
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(state, "mood");
    const cJSON *band = cJSON_GetObjectItemCaseSensitive(state, "band");
    const cJSON *policy_revision = cJSON_GetObjectItemCaseSensitive(state, "policyRevision");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(policy, "deepNightLock");
    const cJSON *time_zone = cJSON_GetObjectItemCaseSensitive(policy, "timeZone");
    const cJSON *server_epoch = cJSON_GetObjectItemCaseSensitive(clock, "serverNowEpochMs");
    const cJSON *valid_until = cJSON_GetObjectItemCaseSensitive(clock, "validUntilEpochMs");
    const cJSON *windows = cJSON_GetObjectItemCaseSensitive(clock, "windowsEpochMs");
    const cJSON *curiosity = cJSON_GetObjectItemCaseSensitive(personality, "curiosity");
    const cJSON *warmth = cJSON_GetObjectItemCaseSensitive(personality, "warmth");
    const cJSON *talkativeness = cJSON_GetObjectItemCaseSensitive(personality, "talkativeness");
    const cJSON *personality_revision = cJSON_GetObjectItemCaseSensitive(personality, "revision");
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    int window_count = cJSON_IsArray(windows) ? cJSON_GetArraySize(windows) : -1;
    if (cJSON_IsString(label) && cJSON_IsString(band) &&
        cJSON_IsNumber(policy_revision) && policy_revision->valueint > 0 &&
        cJSON_IsBool(enabled) && cJSON_IsString(time_zone) &&
        cJSON_IsNumber(server_epoch) &&
        cJSON_IsNumber(valid_until) && window_count >= 0 &&
        window_count <= (int)BABY_TANK_POLICY_MAX_WINDOWS &&
        cJSON_IsNumber(curiosity) && curiosity->valueint >= 0 && curiosity->valueint <= 100 &&
        cJSON_IsNumber(warmth) && warmth->valueint >= 0 && warmth->valueint <= 100 &&
        cJSON_IsNumber(talkativeness) && talkativeness->valueint >= 0 &&
        talkativeness->valueint <= 100 && cJSON_IsNumber(personality_revision) &&
        personality_revision->valueint > 0) {
        baby_tank_policy_window_t parsed[BABY_TANK_POLICY_MAX_WINDOWS] = {0};
        bool valid = true;
        for (int index = 0; index < window_count; ++index) {
            const cJSON *pair = cJSON_GetArrayItem(windows, index);
            const cJSON *start = cJSON_GetArrayItem(pair, 0);
            const cJSON *end = cJSON_GetArrayItem(pair, 1);
            if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) != 2 ||
                !cJSON_IsNumber(start) || !cJSON_IsNumber(end)) {
                valid = false;
                break;
            }
            parsed[index].start_epoch_ms = (int64_t)start->valuedouble;
            parsed[index].end_epoch_ms = (int64_t)end->valuedouble;
        }
        if (valid) {
            result = baby_tank_policy_apply((uint32_t)policy_revision->valueint,
                cJSON_IsTrue(enabled), (int64_t)server_epoch->valuedouble,
                (int64_t)valid_until->valuedouble, parsed,
                (size_t)window_count, time_zone->valuestring);
            if (result == ESP_OK) {
                baby_tank_personality_t traits = {
                    .curiosity = (uint8_t)curiosity->valueint,
                    .warmth = (uint8_t)warmth->valueint,
                    .talkativeness = (uint8_t)talkativeness->valueint,
                };
                result = baby_tank_cloud_set_personality(traits);
            }
            if (result == ESP_OK && atomic_load(&s_personality_revision) !=
                                    (uint32_t)personality_revision->valueint) {
                nvs_handle_t handle;
                result = nvs_open("baby_persona", NVS_READWRITE, &handle);
                if (result == ESP_OK) {
                    result = nvs_set_u32(handle, "revision",
                                         (uint32_t)personality_revision->valueint);
                    if (result == ESP_OK) result = nvs_commit(handle);
                    nvs_close(handle);
                }
                if (result == ESP_OK) atomic_store(&s_personality_revision,
                    (uint32_t)personality_revision->valueint);
            }
            if (result == ESP_OK) result = baby_tank_face_set_mood(label->valuestring, 55U);
            if (result == ESP_OK) result = baby_tank_face_set_time_band(band->valuestring);
            if (result == ESP_OK) cache_character_state(state,
                label->valuestring, band->valuestring);
        }
    }
    cJSON_Delete(root);
    if (result == ESP_OK) {
        char ack_body[192];
        int length = snprintf(ack_body, sizeof(ack_body),
            "{\"policyRevision\":%lu,\"personalityRevision\":%lu,"
            "\"clockQuality\":\"%s\",\"nightLockActive\":%s}",
            (unsigned long)baby_tank_policy_revision(),
            (unsigned long)baby_tank_cloud_personality_revision(),
            baby_tank_policy_clock_quality(),
            baby_tank_policy_is_locked() ? "true" : "false");
        if (length > 0 && length < (int)sizeof(ack_body)) {
            char ack_response[256] = {0};
            cloud_response_t ack = { .buffer = ack_response,
                .capacity = sizeof(ack_response), .callback_result = ESP_OK };
            int ack_status = 0;
            esp_err_t sent = perform_request("/api/device/v2/state/ack",
                HTTP_METHOD_POST, "application/json", ack_body,
                (size_t)length, &ack, &ack_status, 8000);
            if (sent != ESP_OK || ack_status != 200) {
                ESP_LOGW(TAG, "state applied but ack failed: %s status=%d",
                    esp_err_to_name(sent), ack_status);
            }
        }
        ESP_LOGI(TAG, "needs-derived idle mood and policy refreshed");
    }
    return result;
}

esp_err_t baby_tank_cloud_upload_character_event(const char *event_id,
                                                 const char *kind,
                                                 bool *permanent_rejection)
{
    ESP_RETURN_ON_FALSE(s_initialized && event_id != NULL && kind != NULL &&
                        permanent_rejection != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid character event upload");
    *permanent_rejection = false;
    char body[128];
    int length = snprintf(body, sizeof(body),
        "{\"eventId\":\"%s\",\"kind\":\"%s\"}", event_id, kind);
    ESP_RETURN_ON_FALSE(length > 0 && length < (int)sizeof(body),
                        ESP_ERR_INVALID_SIZE, TAG, "character event too large");
    char reply[384] = {0};
    cloud_response_t response = { .buffer = reply, .capacity = sizeof(reply),
                                  .callback_result = ESP_OK };
    int status = 0;
    esp_err_t result = perform_request("/api/device/v2/state/events",
        HTTP_METHOD_POST, "application/json", body, (size_t)length,
        &response, &status, 8000);
    if (result != ESP_OK) return result;
    if (status == 200) return ESP_OK;
    if (status == 400 || status == 423 || status == 429) {
        *permanent_rejection = true;
    }
    ESP_LOGW(TAG, "character event rejected: HTTP %d", status);
    return ESP_ERR_INVALID_RESPONSE;
}

void baby_tank_cloud_cancel_current(void)
{
    atomic_store_explicit(&s_cancel_requested, true, memory_order_relaxed);
}

void baby_tank_cloud_reset_cancel(void)
{
    atomic_store_explicit(&s_cancel_requested, false, memory_order_relaxed);
}

static esp_err_t cloud_converse_request(
    const int16_t *samples, size_t sample_count, const char *touch,
    baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate)
{
    ESP_RETURN_ON_FALSE(audio_callback != NULL &&
                            ((touch == NULL && samples != NULL &&
                              sample_count > 0U &&
                              sample_count <= CLOUD_MAX_PCM_SAMPLES) ||
                             (touch != NULL && samples == NULL &&
                              sample_count == 0U)),
                        ESP_ERR_INVALID_ARG, TAG, "invalid conversation audio");
    baby_tank_personality_t personality = baby_tank_cloud_get_personality();
    char path[144];
    int length = snprintf(path, sizeof(path),
                          "/conversation?format=pcm&sampleRate=16000&"
                          "curiosity=%u&warmth=%u&talkativeness=%u%s%s",
                          personality.curiosity, personality.warmth,
                          personality.talkativeness,
                          touch != NULL ? "&touch=" : "",
                          touch != NULL ? touch : "");
    ESP_RETURN_ON_FALSE(length > 0 && length < (int)sizeof(path),
                        ESP_ERR_INVALID_SIZE, TAG,
                        "conversation URL is too long");
    cloud_response_t response = {
        .audio_callback = audio_callback,
        .metadata_callback = metadata_callback,
        .audio_context = context,
        .callback_result = ESP_OK,
    };
    int status = 0;
    esp_err_t result = perform_request(
        path, HTTP_METHOD_POST, "audio/pcm", samples,
        sample_count * sizeof(*samples), &response, &status, 120000);
    if (result == ESP_OK) result = require_success(status);
    if (format != NULL) *format = response.audio_format;
    if (sample_rate != NULL) *sample_rate = response.audio_sample_rate;
    return result;
}

esp_err_t baby_tank_cloud_converse_pcm16(
    const int16_t *samples, size_t sample_count,
    baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate)
{
    return cloud_converse_request(samples, sample_count, NULL,
                                  metadata_callback, audio_callback, context,
                                  format, sample_rate);
}

static void voice_socket_message(const char *data, size_t length)
{
    if (length == 0U || length >= CLOUD_WS_TEXT_CAPACITY) {
        s_voice_socket_result = ESP_ERR_INVALID_SIZE;
        xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_DONE_BIT);
        return;
    }
    cJSON *root = cJSON_ParseWithLength(data, length);
    if (root == NULL) {
        s_voice_socket_result = ESP_ERR_INVALID_RESPONSE;
        xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_DONE_BIT);
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "hello") == 0) {
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
        if (cJSON_IsNumber(version) && version->valueint == 1 &&
            cJSON_IsString(transport) &&
            strcmp(transport->valuestring, "websocket") == 0) {
            xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_READY_BIT);
            baby_tank_network_cloud_transport_verified();
            ESP_LOGI(TAG, "warm voice WebSocket ready");
        } else {
            xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_FAILED_BIT);
            baby_tank_network_cloud_transport_failed();
        }
    } else if (cJSON_IsString(type) &&
               strcmp(type->valuestring, "reply") == 0 &&
               s_voice_socket_response != NULL) {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        const cJSON *headers = cJSON_GetObjectItemCaseSensitive(root, "headers");
        if (!cJSON_IsNumber(status) || !cJSON_IsObject(headers)) {
            s_voice_socket_result = ESP_ERR_INVALID_RESPONSE;
        } else {
            s_voice_socket_status = status->valueint;
            for (const cJSON *header = headers->child;
                 header != NULL; header = header->next) {
                if (!cJSON_IsString(header) || header->string == NULL) continue;
                esp_http_client_event_t event = {
                    .event_id = HTTP_EVENT_ON_HEADER,
                    .user_data = s_voice_socket_response,
                    .header_key = header->string,
                    .header_value = header->valuestring,
                };
                esp_err_t parsed = cloud_http_event(&event);
                if (parsed != ESP_OK) {
                    s_voice_socket_result = parsed;
                    break;
                }
            }
            if (s_voice_socket_result == ESP_OK && status->valueint == 200) {
                parse_stream_action(s_voice_socket_response,
                                    s_voice_socket_action);
                if (strcmp(s_voice_socket_response->action_type, "drive") == 0) {
                    ESP_LOGI(TAG,
                             "voice drive metadata mode=%s status=%s direction=%s approved=%d",
                             s_voice_socket_response->action_mode,
                             s_voice_socket_response->action_status,
                             s_voice_socket_response->action_direction,
                             s_voice_socket_action != NULL &&
                             s_voice_socket_action->approved ? 1 : 0);
                }
            }
        }
        if (s_voice_socket_result != ESP_OK) {
            xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_DONE_BIT);
        }
    } else if (cJSON_IsString(type) &&
               strcmp(type->valuestring, "text_available") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (cJSON_IsString(id) && valid_explore_trace(id->valuestring)) {
            taskENTER_CRITICAL(&s_text_notice_lock);
            copy_text(s_text_notice_job_id, sizeof(s_text_notice_job_id),
                      id->valuestring);
            taskEXIT_CRITICAL(&s_text_notice_lock);
            atomic_store(&s_text_notice_pending, true);
            ESP_LOGI(TAG, "App text turn received on warm voice socket");
        } else {
            ESP_LOGW(TAG, "invalid App text notice on warm voice socket");
        }
    } else if (cJSON_IsString(type) &&
               strcmp(type->valuestring, "turn_end") == 0 &&
               s_voice_socket_response != NULL) {
        if (s_voice_socket_status == 422) {
            s_voice_socket_result = ESP_ERR_NOT_FOUND;
        } else if (s_voice_socket_result == ESP_OK) {
            s_voice_socket_result = require_success(s_voice_socket_status);
        }
        if (s_voice_socket_result == ESP_OK &&
            (s_voice_socket_action == NULL || !s_voice_socket_action->look) &&
            (s_voice_socket_response->frame_header_used != 0U ||
             s_voice_socket_response->frame_remaining != 0U ||
             s_voice_socket_response->framed_pcm_bytes == 0U ||
             s_voice_socket_response->framed_pcm_bytes % 2U != 0U)) {
            s_voice_socket_result = ESP_ERR_INVALID_RESPONSE;
        }
        xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_DONE_BIT);
    } else if (cJSON_IsString(type) &&
               strcmp(type->valuestring, "turn_error") == 0) {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        s_voice_socket_result = cJSON_IsNumber(status) && status->valueint == 422
            ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_DONE_BIT);
    }
    cJSON_Delete(root);
}

static void voice_socket_event(void *argument, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)base;
    if (s_voice_socket_events == NULL) return;
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
        event_id == WEBSOCKET_EVENT_ERROR ||
        event_id == WEBSOCKET_EVENT_CLOSED) {
        xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_FAILED_BIT);
        baby_tank_network_cloud_transport_failed();
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || event_data == NULL) return;
    const esp_websocket_event_data_t *packet = event_data;
    /* The client also dispatches WebSocket control frames (including the
     * PONGs caused by our keepalive pings). They are not voice data. */
    if (packet->op_code == 0x09 || packet->op_code == 0x0a) return;
    if (packet->data_len <= 0 || packet->payload_offset < 0 ||
        packet->payload_len <= 0 ||
        packet->payload_offset + packet->data_len > packet->payload_len) {
        xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_FAILED_BIT);
        return;
    }
    if (packet->op_code == 0x02) {
        if (s_voice_socket_response == NULL || s_voice_socket_status != 200 ||
            s_voice_socket_result != ESP_OK ||
            atomic_load_explicit(&s_cancel_requested, memory_order_relaxed)) return;
        s_voice_socket_result = parse_framed_audio(
            s_voice_socket_response, (const uint8_t *)packet->data_ptr,
            (size_t)packet->data_len);
        if (s_voice_socket_result != ESP_OK) {
            xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_DONE_BIT);
        }
        return;
    }
    if (packet->op_code != 0x01 ||
        packet->payload_len >= CLOUD_WS_TEXT_CAPACITY) {
        xEventGroupSetBits(s_voice_socket_events, CLOUD_WS_FAILED_BIT);
        baby_tank_network_cloud_transport_failed();
        return;
    }
    memcpy(s_voice_socket_text + packet->payload_offset,
           packet->data_ptr, (size_t)packet->data_len);
    if (packet->payload_offset + packet->data_len == packet->payload_len) {
        voice_socket_message(s_voice_socket_text, (size_t)packet->payload_len);
    }
}

void baby_tank_cloud_voice_session_close(void)
{
    if (s_voice_socket != NULL) {
        esp_websocket_client_stop(s_voice_socket);
        esp_websocket_client_destroy(s_voice_socket);
        s_voice_socket = NULL;
        ESP_LOGI(TAG, "voice WebSocket session closed");
    }
    s_voice_socket_response = NULL;
    s_voice_socket_action = NULL;
    if (s_voice_socket_events != NULL) {
        xEventGroupClearBits(s_voice_socket_events,
                             CLOUD_WS_READY_BIT | CLOUD_WS_DONE_BIT |
                             CLOUD_WS_FAILED_BIT);
    }
}

bool baby_tank_cloud_voice_session_active(void)
{
    if (s_voice_socket == NULL || s_voice_socket_events == NULL) return false;
    EventBits_t bits = xEventGroupGetBits(s_voice_socket_events);
    return (bits & CLOUD_WS_FAILED_BIT) == 0U;
}

bool baby_tank_cloud_text_notice_pending(void)
{
    return atomic_load(&s_text_notice_pending);
}

void baby_tank_cloud_clear_text_notice(void)
{
    atomic_store(&s_text_notice_pending, false);
}

bool baby_tank_cloud_take_text_notice(char *job_id, size_t capacity)
{
    if (job_id == NULL || capacity < sizeof(s_text_notice_job_id) ||
        !atomic_exchange(&s_text_notice_pending, false)) return false;
    taskENTER_CRITICAL(&s_text_notice_lock);
    copy_text(job_id, capacity, s_text_notice_job_id);
    s_text_notice_job_id[0] = '\0';
    taskEXIT_CRITICAL(&s_text_notice_lock);
    return valid_explore_trace(job_id);
}

esp_err_t baby_tank_cloud_voice_session_prepare(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "voice cloud credentials are unavailable");
    if (s_voice_socket != NULL) {
        EventBits_t bits = xEventGroupGetBits(s_voice_socket_events);
        if ((bits & CLOUD_WS_FAILED_BIT) == 0U &&
            ((bits & CLOUD_WS_READY_BIT) == 0U ||
             esp_websocket_client_is_connected(s_voice_socket))) {
            /* A warm connection is still handshaking; do not tear it down
             * when recording starts shortly after the wake acknowledgement. */
            return ESP_OK;
        }
        baby_tank_cloud_voice_session_close();
    }
    if (s_voice_socket_events == NULL) {
        s_voice_socket_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_voice_socket_events != NULL, ESP_ERR_NO_MEM, TAG,
                            "voice WebSocket event allocation failed");
    }
    const char *origin = BABY_TANK_CLOUD_ORIGIN;
    const char *suffix = NULL;
    const char *scheme = NULL;
    if (strncmp(origin, "https://", 8U) == 0) {
        suffix = origin + 8U;
        scheme = "wss://";
    } else if (strncmp(origin, "http://", 7U) == 0) {
        suffix = origin + 7U;
        scheme = "ws://";
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    char uri[CLOUD_URL_CAPACITY];
    int size = snprintf(uri, sizeof(uri), "%s%s/api/device/v1/conversation/ws",
                        scheme, suffix);
    ESP_RETURN_ON_FALSE(size > 0 && size < (int)sizeof(uri),
                        ESP_ERR_INVALID_SIZE, TAG, "voice WebSocket URL too long");
    const esp_websocket_client_config_t config = {
        .uri = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .keep_alive_enable = true,
        .task_stack = 6144,
        /* TLS handshakes are CPU-heavy. Keep them on core 0 so the AFE fetch
         * task on core 1 cannot be starved and lose the start of speech. */
        .task_core_id_set = true,
        .task_core_id = 0,
        .task_prio = 4,
        .buffer_size = 4096,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
    };
    s_voice_socket = esp_websocket_client_init(&config);
    ESP_RETURN_ON_FALSE(s_voice_socket != NULL, ESP_ERR_NO_MEM, TAG,
                        "voice WebSocket client allocation failed");
    char authorization[CLOUD_TOKEN_CAPACITY + 16U];
    snprintf(authorization, sizeof(authorization), "Bearer %s", s_device_token);
    esp_err_t result = esp_websocket_client_append_header(
        s_voice_socket, "Authorization", authorization);
    memset(authorization, 0, sizeof(authorization));
    if (result == ESP_OK) {
        result = esp_websocket_register_events(s_voice_socket,
            WEBSOCKET_EVENT_ANY, voice_socket_event, NULL);
    }
    if (result == ESP_OK) result = esp_websocket_client_start(s_voice_socket);
    if (result != ESP_OK) baby_tank_cloud_voice_session_close();
    return result;
}

static esp_err_t cloud_converse_voice_socket(
    baby_tank_cloud_pcm_source_cb_t source_callback,
    baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_reply_cue_cb_t cue_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate,
    baby_tank_cloud_drive_action_t *action)
{
    esp_err_t result = baby_tank_cloud_voice_session_prepare();
    if (result != ESP_OK) return ESP_ERR_NOT_SUPPORTED;
    EventBits_t bits = xEventGroupWaitBits(s_voice_socket_events,
        CLOUD_WS_READY_BIT | CLOUD_WS_FAILED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(5000));
    if ((bits & CLOUD_WS_READY_BIT) == 0U ||
        (bits & CLOUD_WS_FAILED_BIT) != 0U) {
        baby_tank_cloud_voice_session_close();
        return ESP_ERR_NOT_SUPPORTED;
    }
    cloud_response_t response = {
        .audio_callback = audio_callback,
        .metadata_callback = metadata_callback,
        .cue_callback = cue_callback,
        .audio_context = context,
        .callback_result = ESP_OK,
    };
    s_voice_socket_response = &response;
    s_voice_socket_action = action;
    s_voice_socket_result = ESP_OK;
    s_voice_socket_status = 0;
    xEventGroupClearBits(s_voice_socket_events, CLOUD_WS_DONE_BIT);
    baby_tank_personality_t personality = baby_tank_cloud_get_personality();
    char start[128];
    int start_size = snprintf(start, sizeof(start),
        "{\"type\":\"listen\",\"state\":\"start\","
        "\"curiosity\":%u,\"warmth\":%u,\"talkativeness\":%u}",
        personality.curiosity, personality.warmth, personality.talkativeness);
    if (start_size <= 0 || start_size >= (int)sizeof(start) ||
        esp_websocket_client_send_text(s_voice_socket, start, start_size,
                                       pdMS_TO_TICKS(3000)) != start_size) {
        result = ESP_FAIL;
        goto failure;
    }
    uint8_t *frame = heap_caps_malloc(CLOUD_WS_PCM_FRAME_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame == NULL) {
        result = ESP_ERR_NO_MEM;
        goto failure;
    }
    size_t sent_bytes = 0U;
    int64_t source_wait_us = 0;
    int64_t socket_send_us = 0;
    int64_t max_send_us = 0;
    while (true) {
        size_t frame_size = 0U;
        int64_t began_us = esp_timer_get_time();
        result = source_callback(frame, CLOUD_WS_PCM_FRAME_BYTES,
                                 &frame_size, context);
        source_wait_us += esp_timer_get_time() - began_us;
        if (result != ESP_OK) break;
        if (frame_size == 0U) break;
        if (frame_size > CLOUD_WS_PCM_FRAME_BYTES || frame_size % 2U != 0U ||
            sent_bytes + frame_size > CLOUD_MAX_PCM_SAMPLES * sizeof(int16_t)) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        began_us = esp_timer_get_time();
        int sent = esp_websocket_client_send_bin(s_voice_socket,
            (const char *)frame, (int)frame_size, pdMS_TO_TICKS(3000));
        if (sent != (int)frame_size) {
            ESP_LOGW(TAG, "voice WebSocket PCM send failed wanted=%zu sent=%d connected=%d",
                     frame_size, sent,
                     (int)esp_websocket_client_is_connected(s_voice_socket));
            result = ESP_FAIL;
            break;
        }
        int64_t elapsed_us = esp_timer_get_time() - began_us;
        socket_send_us += elapsed_us;
        if (elapsed_us > max_send_us) max_send_us = elapsed_us;
        sent_bytes += frame_size;
    }
    free(frame);
    if (result != ESP_OK || sent_bytes == 0U) {
        if (result == ESP_OK) result = ESP_ERR_INVALID_SIZE;
        goto failure;
    }
    ESP_LOGI(TAG,
             "WebSocket microphone PCM bytes=%zu source_wait=%lld ms send=%lld ms max_send=%lld ms",
             sent_bytes, (long long)(source_wait_us / 1000),
             (long long)(socket_send_us / 1000),
             (long long)(max_send_us / 1000));
    static const char stop[] = "{\"type\":\"listen\",\"state\":\"stop\"}";
    if (esp_websocket_client_send_text(s_voice_socket, stop,
            sizeof(stop) - 1U, pdMS_TO_TICKS(3000)) != (int)(sizeof(stop) - 1U)) {
        result = ESP_FAIL;
        goto failure;
    }
    for (uint32_t waited_ms = 0U; waited_ms < 120000U; waited_ms += 100U) {
        bits = xEventGroupWaitBits(s_voice_socket_events,
            CLOUD_WS_DONE_BIT | CLOUD_WS_FAILED_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(100));
        if ((bits & CLOUD_WS_DONE_BIT) != 0U) {
            result = s_voice_socket_result;
            break;
        }
        if ((bits & CLOUD_WS_FAILED_BIT) != 0U ||
            atomic_load_explicit(&s_cancel_requested, memory_order_relaxed)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        result = ESP_ERR_TIMEOUT;
    }
    if (format != NULL) *format = response.audio_format;
    if (sample_rate != NULL) *sample_rate = response.audio_sample_rate;
    s_voice_socket_response = NULL;
    s_voice_socket_action = NULL;
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) {
        baby_tank_cloud_voice_session_close();
    }
    return result;

failure:
    s_voice_socket_response = NULL;
    s_voice_socket_action = NULL;
    baby_tank_cloud_voice_session_close();
    return result;
}

esp_err_t baby_tank_cloud_converse_pcm16_stream(
    baby_tank_cloud_pcm_source_cb_t source_callback,
    baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_reply_cue_cb_t cue_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate,
    baby_tank_cloud_drive_action_t *action)
{
    if (action != NULL) memset(action, 0, sizeof(*action));
    ESP_RETURN_ON_FALSE(s_initialized && source_callback != NULL &&
                            audio_callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid streaming conversation");
    esp_err_t socket_result = cloud_converse_voice_socket(
        source_callback, metadata_callback, cue_callback, audio_callback,
        context, format, sample_rate, action);
    if (socket_result != ESP_ERR_NOT_SUPPORTED) return socket_result;
    ESP_LOGW(TAG, "voice WebSocket unavailable before upload; HTTPS fallback");
    baby_tank_personality_t personality = baby_tank_cloud_get_personality();
    char url[CLOUD_URL_CAPACITY];
    int url_length = snprintf(url, sizeof(url),
        BABY_TANK_CLOUD_BASE_URL
        "/conversation/stream?format=pcm&sampleRate=16000&"
        "curiosity=%u&warmth=%u&talkativeness=%u&wire=framed-v1&vision=always",
        personality.curiosity, personality.warmth, personality.talkativeness);
    ESP_RETURN_ON_FALSE(url_length > 0 && url_length < (int)sizeof(url),
                        ESP_ERR_INVALID_SIZE, TAG, "stream URL is too long");

    cloud_response_t response = {
        .audio_callback = audio_callback,
        .metadata_callback = metadata_callback,
        .cue_callback = cue_callback,
        .audio_context = context,
        .callback_result = ESP_OK,
    };
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = cloud_http_event,
        .user_data = &response,
        .timeout_ms = 120000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create streaming HTTP client");
    esp_err_t result = ESP_OK;
    uint8_t *frames = heap_caps_malloc(6400U,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frames == NULL) {
        frames = heap_caps_malloc(6400U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (frames == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    char authorization[CLOUD_TOKEN_CAPACITY + 16U];
    snprintf(authorization, sizeof(authorization), "Bearer %s", s_device_token);
    result = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Authorization", authorization);
    memset(authorization, 0, sizeof(authorization));
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Content-Type", "audio/pcm");
    if (result == ESP_OK) result = esp_http_client_open(client, -1);
    if (result != ESP_OK) goto cleanup;

    uint8_t *current = frames;
    uint8_t *next = frames + 3200U;
    size_t current_size = 0U;
    size_t next_size = 0U;
    size_t total_bytes = 0U;
    result = source_callback(current, 3200U, &current_size, context);
    if (result != ESP_OK) goto cleanup;
    if (current_size == 0U || current_size > 3200U || current_size % 2U != 0U) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    while (true) {
        result = source_callback(next, 3200U, &next_size, context);
        if (result != ESP_OK) goto cleanup;
        if (next_size > 3200U || next_size % 2U != 0U ||
            total_bytes + current_size > CLOUD_MAX_PCM_SAMPLES * sizeof(int16_t)) {
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        if (esp_http_client_chunk_write_begin(client, (int)current_size) != 0 ||
            esp_http_client_write(client, (const char *)current,
                                  (int)current_size) != (int)current_size ||
            esp_http_client_chunk_write_end(client, next_size == 0U) != 0) {
            result = ESP_FAIL;
            goto cleanup;
        }
        total_bytes += current_size;
        if (next_size == 0U) break;
        uint8_t *swap = current;
        current = next;
        next = swap;
        current_size = next_size;
    }
    ESP_LOGI(TAG, "streamed microphone PCM bytes=%zu", total_bytes);

    if (esp_http_client_fetch_headers(client) < 0) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    int status = esp_http_client_get_status_code(client);
    /* This endpoint uses 422 only when ASR produced no words. Ambient noise
     * during an open voice session is not a network or cloud failure. */
    if (status == 422) {
        result = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    result = require_success(status);
    if (result != ESP_OK) goto cleanup;
    /* Headers are complete before the first PCM byte. Parse the action now so
     * voice_runtime can start the motor/light track at playback start. */
    parse_stream_action(&response, action);
    char read_buffer[1024];
    while (!(action != NULL && action->look) &&
           !esp_http_client_is_complete_data_received(client)) {
        int read_size = esp_http_client_read(client, read_buffer,
                                             sizeof(read_buffer));
        if (response.callback_result != ESP_OK) {
            result = response.callback_result;
            break;
        }
        if (read_size <= 0) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }
    if (result == ESP_OK && response.framed_audio &&
        (response.frame_header_used != 0U || response.frame_remaining != 0U ||
         response.framed_pcm_bytes == 0U ||
         response.framed_pcm_bytes % 2U != 0U)) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (format != NULL) *format = response.audio_format;
    if (sample_rate != NULL) *sample_rate = response.audio_sample_rate;

cleanup:
    free(frames);
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t baby_tank_cloud_report_drive_action(
    const baby_tank_cloud_drive_action_t *action,
    const char *result, const char *reason)
{
    ESP_RETURN_ON_FALSE(action != NULL && action->approved &&
                            result != NULL && reason != NULL &&
                            (strcmp(result, "completed") == 0 ||
                             strcmp(result, "blocked") == 0 ||
                             strcmp(result, "failed") == 0),
                        ESP_ERR_INVALID_ARG, TAG, "invalid drive report");
    char body[256];
    int size = snprintf(body, sizeof(body),
        "{\"traceId\":\"%s\",\"direction\":\"%s\","
        "\"result\":\"%s\",\"reason\":\"%s\"}",
        action->trace_id, action->direction, result, reason);
    ESP_RETURN_ON_FALSE(size > 0 && size < (int)sizeof(body),
                        ESP_ERR_INVALID_SIZE, TAG, "drive report too large");
    int status = 0;
    esp_err_t sent = perform_request("/ai-action/result", HTTP_METHOD_POST,
                                    "application/json", body, (size_t)size,
                                    NULL, &status, 5000);
    memset(body, 0, sizeof(body));
    if (sent != ESP_OK) return sent;
    return require_success(status);
}

static bool valid_explore_trace(const char *trace_id)
{
    if (trace_id == NULL || strlen(trace_id) != 36U) return false;
    for (size_t i = 0U; i < 36U; ++i) {
        if (i == 8U || i == 13U || i == 18U || i == 23U) {
            if (trace_id[i] != '-') return false;
        } else if (!((trace_id[i] >= '0' && trace_id[i] <= '9') ||
                     (trace_id[i] >= 'a' && trace_id[i] <= 'f'))) return false;
    }
    return true;
}

esp_err_t baby_tank_cloud_claim_text_turn(char *job_id, size_t capacity)
{
    ESP_RETURN_ON_FALSE(job_id != NULL && capacity >= 40U,
                        ESP_ERR_INVALID_ARG, TAG, "text job buffer too small");
    job_id[0] = '\0';
    char body[128] = {0};
    cloud_response_t response = {
        .buffer = body, .capacity = sizeof(body), .callback_result = ESP_OK,
    };
    int status = 0;
    esp_err_t result = perform_request("/text-turns/next?wait=0", HTTP_METHOD_GET,
        NULL, NULL, 0U, &response, &status, 8000);
    if (result != ESP_OK) return result;
    if (status == 204) return ESP_ERR_NOT_FOUND;
    ESP_RETURN_ON_ERROR(require_success(status), TAG, "text job claim failed");
    ESP_RETURN_ON_FALSE(!response.overflow, ESP_ERR_INVALID_SIZE, TAG,
                        "text job response too large");
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid text job response");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    bool valid = cJSON_IsString(id) && valid_explore_trace(id->valuestring);
    if (valid) copy_text(job_id, capacity, id->valuestring);
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

void baby_tank_cloud_cancel_text_wait(void)
{
    atomic_store(&s_text_wait_cancel, true);
}

esp_err_t baby_tank_cloud_wait_text_idle(uint32_t timeout_ms)
{
    baby_tank_cloud_cancel_text_wait();
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (atomic_load(&s_text_wait_active)) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

esp_err_t baby_tank_cloud_wait_text_turn(char *job_id, size_t capacity,
                                        bool (*can_wait)(void))
{
    ESP_RETURN_ON_FALSE(job_id != NULL && capacity >= 40U &&
                            can_wait != NULL && s_initialized,
                        ESP_ERR_INVALID_ARG, TAG, "invalid text waiter");
    job_id[0] = '\0';
    if (!can_wait()) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_text_wait_cancel, false);
    atomic_store(&s_text_wait_active, true);

    char body[128] = {0};
    cloud_response_t response = {
        .buffer = body, .capacity = sizeof(body), .callback_result = ESP_OK,
    };
    char url[CLOUD_URL_CAPACITY];
    int url_length = snprintf(url, sizeof(url),
                              "%s/text-turns/next?waitMs=250",
                              BABY_TANK_CLOUD_BASE_URL);
    /* A single client survives short empty responses. The server returns at
     * least every 250 ms so foreground voice can preempt the waiter without
     * blocking an AFE consumer or tearing a TLS handle down cross-task. */
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = cloud_http_event,
        .user_data = &response,
        /* The first TLS handshake can take several seconds on this board.
         * Once connected, the server's 250 ms bounded response supplies the
         * foreground preemption point; this timeout is only a hard failure
         * ceiling and must not kill a healthy initial handshake. */
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2,
        .keep_alive_enable = true,
        .is_async = true,
    };
    esp_err_t result = ESP_OK;
    esp_http_client_handle_t client = NULL;
    char authorization[CLOUD_TOKEN_CAPACITY + 16U];
    snprintf(authorization, sizeof(authorization), "Bearer %s", s_device_token);
    if (url_length <= 0 || url_length >= (int)sizeof(url)) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    client = esp_http_client_init(&config);
    if (client == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    result = esp_http_client_set_method(client, HTTP_METHOD_GET);
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "Authorization", authorization);
    }
    while (result == ESP_OK && !atomic_load(&s_text_wait_cancel) &&
           can_wait()) {
        response.length = 0U;
        response.overflow = false;
        response.callback_result = ESP_OK;
        body[0] = '\0';
        int64_t started_us = esp_timer_get_time();
        do {
            result = esp_http_client_perform(client);
            if (result == ESP_ERR_HTTP_EAGAIN) {
                if (atomic_load(&s_text_wait_cancel) || !can_wait()) {
                    result = ESP_ERR_INVALID_STATE;
                    break;
                }
                if (esp_timer_get_time() - started_us > 10000000LL) {
                    result = ESP_ERR_TIMEOUT;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } while (result == ESP_ERR_HTTP_EAGAIN);
        if (result != ESP_OK) break;
        if (response.overflow || response.callback_result != ESP_OK) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        int status = esp_http_client_get_status_code(client);
        if (status == 204) continue;
        if (status != 200) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        cJSON *root = cJSON_Parse(body);
        if (root == NULL) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        bool valid = cJSON_IsString(id) && valid_explore_trace(id->valuestring);
        if (valid) copy_text(job_id, capacity, id->valuestring);
        cJSON_Delete(root);
        result = valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        if (result == ESP_OK) {
            /* Ownership moves to the dialogue task. Reusing this authenticated
             * keep-alive connection removes a second 4-6 second TLS setup from
             * App text turns. No I/O occurs between these two task contexts. */
            (void)esp_http_client_set_user_data(client, NULL);
            save_text_claim_client(client);
            client = NULL;
        }
        goto cleanup;
    }
    if (result == ESP_OK) result = ESP_ERR_INVALID_STATE;

cleanup:
    memset(authorization, 0, sizeof(authorization));
    if (client != NULL) esp_http_client_cleanup(client);
    atomic_store(&s_text_wait_active, false);
    return result;
}

static esp_err_t cloud_converse_text_socket(
    const char *job_id, baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_reply_cue_cb_t cue_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate,
    baby_tank_cloud_drive_action_t *action)
{
    if (!baby_tank_cloud_voice_session_active()) return ESP_ERR_NOT_SUPPORTED;
    EventBits_t bits = xEventGroupWaitBits(s_voice_socket_events,
        CLOUD_WS_READY_BIT | CLOUD_WS_FAILED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(1000));
    if ((bits & CLOUD_WS_READY_BIT) == 0U ||
        (bits & CLOUD_WS_FAILED_BIT) != 0U ||
        !esp_websocket_client_is_connected(s_voice_socket)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    cloud_response_t response = {
        .audio_callback = audio_callback,
        .metadata_callback = metadata_callback,
        .cue_callback = cue_callback,
        .audio_context = context,
        .callback_result = ESP_OK,
    };
    s_voice_socket_response = &response;
    s_voice_socket_action = action;
    s_voice_socket_result = ESP_OK;
    s_voice_socket_status = 0;
    xEventGroupClearBits(s_voice_socket_events, CLOUD_WS_DONE_BIT);
    char start[112];
    int start_size = snprintf(start, sizeof(start),
        "{\"type\":\"text\",\"state\":\"start\",\"jobId\":\"%s\"}",
        job_id);
    esp_err_t result = ESP_OK;
    if (start_size <= 0 || start_size >= (int)sizeof(start) ||
        esp_websocket_client_send_text(s_voice_socket, start, start_size,
                                       pdMS_TO_TICKS(3000)) != start_size) {
        result = ESP_FAIL;
        goto cleanup;
    }
    for (uint32_t waited_ms = 0U; waited_ms < 120000U; waited_ms += 100U) {
        bits = xEventGroupWaitBits(s_voice_socket_events,
            CLOUD_WS_DONE_BIT | CLOUD_WS_FAILED_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(100));
        if ((bits & CLOUD_WS_DONE_BIT) != 0U) {
            result = s_voice_socket_result;
            break;
        }
        if ((bits & CLOUD_WS_FAILED_BIT) != 0U ||
            atomic_load_explicit(&s_cancel_requested, memory_order_relaxed)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        result = ESP_ERR_TIMEOUT;
    }
    if (format != NULL) *format = response.audio_format;
    if (sample_rate != NULL) *sample_rate = response.audio_sample_rate;

cleanup:
    s_voice_socket_response = NULL;
    s_voice_socket_action = NULL;
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) {
        baby_tank_cloud_voice_session_close();
    }
    return result;
}

esp_err_t baby_tank_cloud_converse_text_turn(
    const char *job_id, baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_reply_cue_cb_t cue_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate,
    baby_tank_cloud_drive_action_t *action)
{
    ESP_RETURN_ON_FALSE(valid_explore_trace(job_id) && audio_callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid text turn");
    if (action != NULL) memset(action, 0, sizeof(*action));
    esp_err_t socket_result = cloud_converse_text_socket(
        job_id, metadata_callback, cue_callback, audio_callback, context,
        format, sample_rate, action);
    if (socket_result != ESP_ERR_NOT_SUPPORTED) return socket_result;
    ESP_LOGW(TAG, "warm text WebSocket unavailable; HTTPS fallback");
    char path[144];
    int length = snprintf(path, sizeof(path),
        "/conversation/text?jobId=%s&wire=framed-v1&vision=always", job_id);
    ESP_RETURN_ON_FALSE(length > 0 && length < (int)sizeof(path),
                        ESP_ERR_INVALID_SIZE, TAG, "text turn path too long");
    cloud_response_t response = {
        .audio_callback = audio_callback,
        .metadata_callback = metadata_callback,
        .cue_callback = cue_callback,
        .audio_context = context,
        .callback_result = ESP_OK,
    };
    int status = 0;
    esp_http_client_handle_t claimed_client = take_text_claim_client();
    esp_err_t result;
    if (claimed_client != NULL) {
        int64_t started_us = esp_timer_get_time();
        result = perform_reused_text_request(claimed_client, path, &response,
                                             &status, 120000);
        ESP_LOGI(TAG,
                 "text turn reused claim TLS connection start=%lld ms result=%s",
                 (long long)((esp_timer_get_time() - started_us) / 1000LL),
                 esp_err_to_name(result));
        esp_http_client_cleanup(claimed_client);
        /* Retry only before any reply metadata/audio was delivered. Replaying
         * after playback began would duplicate speech and possibly an action. */
        if (result != ESP_OK && !response.metadata_delivered &&
            response.framed_pcm_bytes == 0U) {
            memset(&response, 0, sizeof(response));
            response.audio_callback = audio_callback;
            response.metadata_callback = metadata_callback;
            response.cue_callback = cue_callback;
            response.audio_context = context;
            response.callback_result = ESP_OK;
            status = 0;
            result = perform_request(path, HTTP_METHOD_POST, NULL,
                                     NULL, 0U, &response, &status, 120000);
        }
    } else {
        result = perform_request(path, HTTP_METHOD_POST, NULL,
                                 NULL, 0U, &response, &status, 120000);
    }
    if (result == ESP_OK) result = require_success(status);
    if (result == ESP_OK) {
        parse_stream_action(&response, action);
        if (strcmp(response.action_type, "drive") == 0) {
            ESP_LOGI(TAG,
                     "text drive headers version=%s mode=%s status=%s direction=%s approved=%d",
                     response.action_version, response.action_mode,
                     response.action_status, response.action_direction,
                     action != NULL && action->approved ? 1 : 0);
        }
    }
    if (result == ESP_OK && (action == NULL || !action->look) &&
        (!response.framed_audio || response.frame_header_used != 0U ||
         response.frame_remaining != 0U ||
         response.framed_pcm_bytes == 0U ||
         response.framed_pcm_bytes % 2U != 0U)) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (format != NULL) *format = response.audio_format;
    if (sample_rate != NULL) *sample_rate = response.audio_sample_rate;
    return result;
}

esp_err_t baby_tank_cloud_complete_text_turn(const char *job_id,
                                             const char *status)
{
    ESP_RETURN_ON_FALSE(valid_explore_trace(job_id) && status != NULL &&
        (strcmp(status, "completed") == 0 || strcmp(status, "failed") == 0 ||
         strcmp(status, "blocked") == 0), ESP_ERR_INVALID_ARG, TAG,
                        "invalid text turn completion");
    /* A claimed job that could not enter the dialogue queue must not leave
     * its keep-alive/TLS context resident in the scarce internal heap. */
    discard_text_claim_client();
    char path[96];
    char body[48];
    snprintf(path, sizeof(path), "/text-turns/%s/complete", job_id);
    int size = snprintf(body, sizeof(body), "{\"status\":\"%s\"}", status);
    int http_status = 0;
    esp_err_t result = perform_request(path, HTTP_METHOD_POST,
        "application/json", body, (size_t)size, NULL, &http_status, 5000);
    if (result == ESP_OK) result = require_success(http_status);
    return result;
}

esp_err_t baby_tank_cloud_fetch_drawing(const char *trace_id,
                                        baby_tank_drawing_t *drawing)
{
    ESP_RETURN_ON_FALSE(valid_explore_trace(trace_id) && drawing != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid drawing request");
    char path[96];
    int path_length = snprintf(path, sizeof(path),
                               "/drawing?traceId=%s", trace_id);
    ESP_RETURN_ON_FALSE(path_length > 0 && path_length < (int)sizeof(path),
                        ESP_ERR_INVALID_SIZE, TAG, "drawing path too long");
    uint8_t *jpeg = heap_caps_malloc(CLOUD_DRAWING_JPEG_CAPACITY + 1U,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(jpeg != NULL, ESP_ERR_NO_MEM, TAG,
                        "drawing JPEG allocation failed");
    cloud_response_t response = {
        .buffer = (char *)jpeg,
        .capacity = CLOUD_DRAWING_JPEG_CAPACITY + 1U,
        .callback_result = ESP_OK,
    };
    int status = 0;
    esp_err_t result = perform_request(path, HTTP_METHOD_GET, NULL,
                                       NULL, 0U, &response, &status, 15000);
    if (result == ESP_OK) result = require_success(status);
    if (result == ESP_OK && response.overflow) result = ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK &&
        strncasecmp(response.content_type, "image/jpeg", 10U) != 0) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    memset(drawing, 0, sizeof(*drawing));
    char *duration_end = NULL;
    unsigned long duration = strtoul(response.drawing_duration,
                                     &duration_end, 10);
    if (result == ESP_OK &&
        (response.drawing_duration[0] == '\0' || *duration_end != '\0' ||
         duration < 4000UL || duration > BABY_TANK_DRAWING_MAX_DURATION_MS ||
         response.length < 256U)) {
        result = ESP_ERR_INVALID_RESPONSE;
    }

    jpeg_dec_handle_t decoder = NULL;
    if (result == ESP_OK) {
        jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
        config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) result = ESP_FAIL;
    }
    jpeg_dec_io_t io = {
        .inbuf = jpeg,
        .inbuf_len = (int)response.length,
    };
    jpeg_dec_header_info_t info = {0};
    if (result == ESP_OK &&
        jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    const size_t pixel_count = BABY_TANK_DRAWING_WIDTH *
                               BABY_TANK_DRAWING_HEIGHT;
    if (result == ESP_OK &&
        (info.width != BABY_TANK_DRAWING_WIDTH ||
         info.height != BABY_TANK_DRAWING_HEIGHT)) {
        result = ESP_ERR_INVALID_SIZE;
    }
    if (result == ESP_OK) {
        drawing->pixels = heap_caps_aligned_calloc(
            16U, pixel_count, sizeof(*drawing->pixels),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (drawing->pixels == NULL) result = ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK) {
        io.outbuf = (uint8_t *)drawing->pixels;
        if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK ||
            io.out_size != (int)(pixel_count * sizeof(*drawing->pixels))) {
            result = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (decoder != NULL) jpeg_dec_close(decoder);
    heap_caps_free(jpeg);
    if (result != ESP_OK) {
        heap_caps_free(drawing->pixels);
        memset(drawing, 0, sizeof(*drawing));
        return result;
    }
    drawing->duration_ms = (uint16_t)duration;
    drawing->width = info.width;
    drawing->height = info.height;
    drawing->pixel_count = pixel_count;
    ESP_LOGI(TAG, "SVG drawing decoded jpeg=%zu bytes pixels=%zu",
             response.length, pixel_count);
    return ESP_OK;
}

esp_err_t baby_tank_cloud_vision_describe(
    const char *trace_id, const uint8_t *jpeg, size_t jpeg_size,
    baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate)
{
    ESP_RETURN_ON_FALSE(valid_explore_trace(trace_id) && jpeg != NULL &&
                            jpeg_size >= 128U && jpeg_size <= 160U * 1024U &&
                            audio_callback != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid vision request");
    char path[96];
    int length = snprintf(path, sizeof(path),
                          "/vision/describe?traceId=%s", trace_id);
    ESP_RETURN_ON_FALSE(length > 0 && length < (int)sizeof(path),
                        ESP_ERR_INVALID_SIZE, TAG, "vision path too long");
    cloud_response_t response = {
        .audio_callback = audio_callback,
        .metadata_callback = metadata_callback,
        .audio_context = context,
        .callback_result = ESP_OK,
    };
    int status = 0;
    ESP_RETURN_ON_ERROR(perform_request(path, HTTP_METHOD_POST, "image/jpeg",
                                        jpeg, jpeg_size, &response, &status,
                                        120000), TAG, "vision request failed");
    ESP_RETURN_ON_ERROR(require_success(status), TAG, "vision request rejected");
    if (format != NULL) *format = response.audio_format;
    if (sample_rate != NULL) *sample_rate = response.audio_sample_rate;
    return ESP_OK;
}

esp_err_t baby_tank_cloud_explore_observe(
    const char *trace_id, uint8_t step_index, const uint8_t *jpeg,
    size_t jpeg_size, baby_tank_cloud_explore_step_t *decision)
{
    ESP_RETURN_ON_FALSE(valid_explore_trace(trace_id) && step_index < 3U &&
                            jpeg != NULL && jpeg_size >= 128U &&
                            jpeg_size <= 160U * 1024U && decision != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid explore observation");
    memset(decision, 0, sizeof(*decision));
    char path[128];
    int path_length = snprintf(path, sizeof(path),
                               "/explore/observe?traceId=%s&step=%u",
                               trace_id, (unsigned)step_index);
    ESP_RETURN_ON_FALSE(path_length > 0 && path_length < (int)sizeof(path),
                        ESP_ERR_INVALID_SIZE, TAG, "explore path too long");
    char body[1024] = {0};
    cloud_response_t response = {
        .buffer = body, .capacity = sizeof(body), .callback_result = ESP_OK,
    };
    int status = 0;
    ESP_RETURN_ON_ERROR(perform_request(path, HTTP_METHOD_POST, "image/jpeg",
                                        jpeg, jpeg_size, &response, &status,
                                        120000), TAG, "explore observation failed");
    ESP_RETURN_ON_ERROR(require_success(status), TAG, "explore observation rejected");
    ESP_RETURN_ON_FALSE(!response.overflow, ESP_ERR_INVALID_SIZE, TAG,
                        "explore response too large");
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid explore JSON");
    cJSON *trace = cJSON_GetObjectItemCaseSensitive(root, "traceId");
    cJSON *step = cJSON_GetObjectItemCaseSensitive(root, "step");
    cJSON *move = cJSON_GetObjectItemCaseSensitive(root, "move");
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "durationMs");
    cJSON *light = cJSON_GetObjectItemCaseSensitive(root, "lightMode");
    cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    cJSON *intensity = cJSON_GetObjectItemCaseSensitive(root, "intensity");
    bool valid = cJSON_IsString(trace) && strcmp(trace->valuestring, trace_id) == 0 &&
                 cJSON_IsNumber(step) && step->valueint == step_index &&
                 cJSON_IsString(move) && cJSON_IsNumber(duration) &&
                 cJSON_IsString(light) && cJSON_IsString(emotion) &&
                 cJSON_IsNumber(intensity) && intensity->valuedouble >= 20 &&
                 intensity->valuedouble <= 100;
    baby_tank_expression_t checked_expression;
    if (valid) valid = baby_tank_face_expression_from_name(
        emotion->valuestring, &checked_expression) == ESP_OK;
    if (valid) {
        decision->stop = strcmp(move->valuestring, "stop") == 0;
        valid = decision->stop || strcmp(move->valuestring, "forward") == 0 ||
                strcmp(move->valuestring, "backward") == 0 ||
                strcmp(move->valuestring, "left") == 0 ||
                strcmp(move->valuestring, "right") == 0;
        valid = valid && ((decision->stop && duration->valueint == 0) ||
                          (!decision->stop && duration->valueint >= 2000 &&
                           duration->valueint <= 9000));
    }
    if (valid) {
        if (strcmp(light->valuestring, "unchanged") == 0) decision->light_mode = 0U;
        else if (strcmp(light->valuestring, "white") == 0) decision->light_mode = 2U;
        else if (strcmp(light->valuestring, "rainbow") == 0) decision->light_mode = 6U;
        else if (strcmp(light->valuestring, "sleep") == 0) decision->light_mode = 8U;
        else valid = false;
    }
    if (valid) {
        copy_text(decision->direction, sizeof(decision->direction), move->valuestring);
        decision->duration_ms = (uint16_t)duration->valueint;
        copy_text(decision->emotion, sizeof(decision->emotion), emotion->valuestring);
        decision->intensity = (uint8_t)intensity->valueint;
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t baby_tank_cloud_explore_report(
    const char *trace_id, uint8_t step_index, const char *direction,
    const char *result, const char *reason, bool *finished)
{
    ESP_RETURN_ON_FALSE(valid_explore_trace(trace_id) && step_index < 3U &&
                            direction != NULL && result != NULL && reason != NULL &&
                            finished != NULL &&
                            (strcmp(result, "completed") == 0 ||
                             strcmp(result, "blocked") == 0 ||
                             strcmp(result, "failed") == 0),
                        ESP_ERR_INVALID_ARG, TAG, "invalid explore report");
    char payload[256];
    int length = snprintf(payload, sizeof(payload),
        "{\"traceId\":\"%s\",\"step\":%u,\"direction\":\"%s\","
        "\"result\":\"%s\",\"reason\":\"%s\"}", trace_id,
        (unsigned)step_index, direction, result, reason);
    ESP_RETURN_ON_FALSE(length > 0 && length < (int)sizeof(payload),
                        ESP_ERR_INVALID_SIZE, TAG, "explore report too large");
    char body[256] = {0};
    cloud_response_t response = {
        .buffer = body, .capacity = sizeof(body), .callback_result = ESP_OK,
    };
    int status = 0;
    ESP_RETURN_ON_ERROR(perform_request("/explore/result", HTTP_METHOD_POST,
                                        "application/json", payload,
                                        (size_t)length, &response, &status,
                                        10000), TAG, "explore report failed");
    ESP_RETURN_ON_ERROR(require_success(status), TAG, "explore report rejected");
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid explore report JSON");
    cJSON *is_finished = cJSON_GetObjectItemCaseSensitive(root, "finished");
    bool valid = cJSON_IsBool(is_finished);
    if (valid) *finished = cJSON_IsTrue(is_finished);
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t baby_tank_cloud_converse_touch(
    bool left, baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate)
{
    return cloud_converse_request(NULL, 0U, left ? "left" : "right",
                                  metadata_callback, audio_callback, context,
                                  format, sample_rate);
}
