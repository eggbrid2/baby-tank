#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t baby_tank_voice_runtime_start(void);
esp_err_t baby_tank_voice_runtime_stop(void);
bool baby_tank_voice_runtime_is_ready(void);

/* Reports actual stack headroom and heap state for memory tuning. */
void baby_tank_voice_runtime_log_memory(void);

/* Safe from ordinary FreeRTOS task context, including the touch task. */
esp_err_t baby_tank_voice_runtime_request_listen(void);

/* Explicit App stop action. Cancels capture, cloud work, playback and any
 * foreground AI action, then returns to local wake-word standby. */
esp_err_t baby_tank_voice_runtime_cancel_conversation(void);

/* Ends any active foreground interaction and enters ordinary, wakeable
 * sleep. Unlike the deep-night gate, WakeNet and top-touch wake remain
 * available. The chassis is stopped before the sleeping face is shown. */
esp_err_t baby_tank_voice_runtime_request_sleep(void);

/* A cloud-relayed typed turn bypasses AFE/ASR but uses the same response,
 * expression, TTS, vision and bounded action runtime. It never auto-opens
 * the microphone after playback. */
bool baby_tank_voice_runtime_can_accept_text(void);
esp_err_t baby_tank_voice_runtime_request_cloud_text(const char *job_id);

/* Top-touch interaction: idle starts a session. Active dialogue ignores a
 * second capacitive edge because it may be coupled noise; the App uses the
 * explicit cancel API above when the user requests an exit. */
esp_err_t baby_tank_voice_runtime_toggle_listen(void);

/* Plays one of the short PCM phrases generated with the configured cloud TTS.
 * Phrase 0 is “我在呢”, phrase 1 is “来啦”, phrase 2 announces remote mode;
 * phrases 6-8 are the fixed game results “我赢了／我输了／平局啦”.
 * The call is synchronous and must run from a worker task. */
esp_err_t baby_tank_voice_runtime_play_phrase(uint8_t phrase);

/* Remote control holds a renewable five-second lease. Entering it destroys
 * the AFE/tasks and starts the camera stream; leaving it does the reverse. */
esp_err_t baby_tank_voice_runtime_set_remote_mode(bool enabled);
bool baby_tank_voice_runtime_remote_mode(void);
/* Cancels active child interaction on the 23:00 policy transition. */
void baby_tank_voice_runtime_enter_night_lock(void);
void baby_tank_voice_runtime_exit_night_lock(void);

typedef enum {
    BABY_TANK_TOUCH_REACTION_LEFT = 0,
    BABY_TANK_TOUCH_REACTION_RIGHT,
} baby_tank_touch_reaction_t;

/* Queues an offline side-personality reaction. Ordinary touches draw from
 * non-repeating left/right pools; rapid repeated touches escalate locally. */
esp_err_t baby_tank_voice_runtime_request_touch_reaction(
    baby_tank_touch_reaction_t side);
