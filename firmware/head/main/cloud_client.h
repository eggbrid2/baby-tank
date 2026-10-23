#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "display.h"

#define BABY_TANK_CLOUD_ORIGIN "https://www.xydocshare.com"
#define BABY_TANK_CLOUD_BASE_URL BABY_TANK_CLOUD_ORIGIN "/api/device/v1"

typedef struct {
    bool available;
    char provider[24];
    char model[64];
} baby_tank_cloud_service_t;

typedef enum {
    BABY_TANK_CLOUD_AUDIO_UNKNOWN = 0,
    BABY_TANK_CLOUD_AUDIO_PCM_S16LE,
    BABY_TANK_CLOUD_AUDIO_MP3,
    BABY_TANK_CLOUD_AUDIO_OGG_OPUS,
} baby_tank_cloud_audio_format_t;

typedef struct {
    char revision[40];
    baby_tank_cloud_service_t llm;
    baby_tank_cloud_service_t asr;
    baby_tank_cloud_service_t tts;
    baby_tank_cloud_audio_format_t tts_format;
    uint32_t tts_sample_rate;
} baby_tank_cloud_runtime_config_t;

typedef struct {
    uint8_t curiosity;
    uint8_t warmth;
    uint8_t talkativeness;
} baby_tank_personality_t;

typedef struct {
    bool approved;
    bool explore;
    bool look;
    bool draw;
    bool sleep_session;
    char trace_id[40];
    char direction[12];
    uint16_t speed_per_mille;
    uint16_t duration_ms;
    uint8_t light_mode; /* 0 unchanged, 2 white, 6 rainbow, 8 sleep */
} baby_tank_cloud_drive_action_t;

typedef struct {
    bool stop;
    char direction[12];
    uint16_t duration_ms;
    uint8_t light_mode; /* 0 unchanged, 2 white, 6 rainbow, 8 sleep */
    char emotion[24];
    uint8_t intensity;
} baby_tank_cloud_explore_step_t;

esp_err_t baby_tank_cloud_set_personality(baby_tank_personality_t personality);
baby_tank_personality_t baby_tank_cloud_get_personality(void);
uint32_t baby_tank_cloud_personality_revision(void);

typedef esp_err_t (*baby_tank_cloud_audio_chunk_cb_t)(
    const uint8_t *data,
    size_t size,
    baby_tank_cloud_audio_format_t format,
    uint32_t sample_rate,
    void *context);

typedef esp_err_t (*baby_tank_cloud_reply_meta_cb_t)(
    const char *emotion, uint8_t intensity,
    const char *face_cues, const char *mood, uint8_t mood_intensity,
    const char *thought, const char *thought_text, void *context);

typedef esp_err_t (*baby_tank_cloud_reply_cue_cb_t)(
    size_t sample_offset, const char *emotion, uint8_t intensity,
    void *context);

/* Supplies the next PCM frame; size=0 marks the end of the utterance. */
typedef esp_err_t (*baby_tank_cloud_pcm_source_cb_t)(
    uint8_t *buffer, size_t capacity, size_t *size, void *context);

/* Copies the token into RAM. The caller must load it from protected NVS. */
esp_err_t baby_tank_cloud_init(const char *device_token);
esp_err_t baby_tank_cloud_fetch_verified_identity(void);
esp_err_t baby_tank_cloud_copy_verified_device_id(char *output, size_t capacity);

esp_err_t baby_tank_cloud_fetch_config(
    baby_tank_cloud_runtime_config_t *runtime_config);

/* Restores the last validated dream caption, then occasionally refreshes it
 * from the cloud mood endpoint while the device is sleeping. */
esp_err_t baby_tank_cloud_restore_sleep_thought(void);
esp_err_t baby_tank_cloud_refresh_sleep_thought(void);
/* Refresh the needs-derived long-term idle expression from the cloud. */
esp_err_t baby_tank_cloud_refresh_character_mood(void);
esp_err_t baby_tank_cloud_upload_character_event(const char *event_id,
                                                 const char *kind,
                                                 bool *permanent_rejection);

/* Cooperative cancellation for the current HTTP exchange. */
void baby_tank_cloud_cancel_current(void);
void baby_tank_cloud_reset_cancel(void);

/* Warm the bidirectional voice channel while the local wake clip plays.
 * A failed warm-up leaves the existing HTTPS conversation path available. */
esp_err_t baby_tank_cloud_voice_session_prepare(void);
void baby_tank_cloud_voice_session_close(void);
bool baby_tank_cloud_voice_session_active(void);
bool baby_tank_cloud_text_notice_pending(void);
void baby_tank_cloud_clear_text_notice(void);
bool baby_tank_cloud_take_text_notice(char *job_id, size_t capacity);

/* Uploads one utterance; the cloud performs ASR, dialogue, and TTS. */
esp_err_t baby_tank_cloud_converse_pcm16(
    const int16_t *samples, size_t sample_count,
    baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate);

/* Uploads microphone frames while they are captured, then reads one PCM reply. */
esp_err_t baby_tank_cloud_converse_pcm16_stream(
    baby_tank_cloud_pcm_source_cb_t source_callback,
    baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_reply_cue_cb_t cue_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate,
    baby_tank_cloud_drive_action_t *action);

/* Cloud-relayed App text enters the same post-ASR dialogue/TTS path. The
 * head never receives the child's typed text through a local network. */
esp_err_t baby_tank_cloud_claim_text_turn(char *job_id, size_t capacity);
/* Keep one HTTPS request pending while the voice runtime is idle. The
 * predicate is checked between non-blocking I/O steps, so wake/camera can
 * preempt this transport before opening their own cloud connection. */
esp_err_t baby_tank_cloud_wait_text_turn(char *job_id, size_t capacity,
                                        bool (*can_wait)(void));
void baby_tank_cloud_cancel_text_wait(void);
esp_err_t baby_tank_cloud_wait_text_idle(uint32_t timeout_ms);
esp_err_t baby_tank_cloud_converse_text_turn(
    const char *job_id, baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_reply_cue_cb_t cue_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate,
    baby_tank_cloud_drive_action_t *action);
esp_err_t baby_tank_cloud_complete_text_turn(const char *job_id,
                                             const char *status);

esp_err_t baby_tank_cloud_report_drive_action(
    const baby_tank_cloud_drive_action_t *action,
    const char *result, const char *reason);

esp_err_t baby_tank_cloud_explore_observe(
    const char *trace_id, uint8_t step_index, const uint8_t *jpeg,
    size_t jpeg_size, baby_tank_cloud_explore_step_t *decision);
/* One photo requested by this turn; only a spoken answer is returned. */
esp_err_t baby_tank_cloud_vision_describe(
    const char *trace_id, const uint8_t *jpeg, size_t jpeg_size,
    baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate);
esp_err_t baby_tank_cloud_fetch_drawing(const char *trace_id,
                                        baby_tank_drawing_t *drawing);
esp_err_t baby_tank_cloud_explore_report(
    const char *trace_id, uint8_t step_index, const char *direction,
    const char *result, const char *reason, bool *finished);

/* Side-touch events use the same cloud dialogue and TTS pipeline, sans ASR. */
esp_err_t baby_tank_cloud_converse_touch(
    bool left, baby_tank_cloud_reply_meta_cb_t metadata_callback,
    baby_tank_cloud_audio_chunk_cb_t audio_callback, void *context,
    baby_tank_cloud_audio_format_t *format, uint32_t *sample_rate);
