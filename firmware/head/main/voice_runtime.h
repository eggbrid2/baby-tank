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

/* Top-touch interaction: idle starts a session; any active dialogue state
 * cancels the cloud exchange and returns to local wake listening. */
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

typedef enum {
    BABY_TANK_TOUCH_REACTION_LEFT = 0,
    BABY_TANK_TOUCH_REACTION_RIGHT,
} baby_tank_touch_reaction_t;

/* Queues an offline side-touch reaction with a bounded escalating expression. */
esp_err_t baby_tank_voice_runtime_request_touch_reaction(
    baby_tank_touch_reaction_t side);
