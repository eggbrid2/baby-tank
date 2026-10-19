#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "display.h"

typedef enum {
    BABY_TANK_CONVERSATION_IDLE = 0,
    BABY_TANK_CONVERSATION_LISTENING,
    BABY_TANK_CONVERSATION_USER_SPEAKING,
    BABY_TANK_CONVERSATION_RECOGNIZING,
    BABY_TANK_CONVERSATION_THINKING,
    BABY_TANK_CONVERSATION_AI_SPEAKING,
    BABY_TANK_CONVERSATION_CANCELLED,
    BABY_TANK_CONVERSATION_ERROR,
    BABY_TANK_CONVERSATION_DISCONNECTED,
} baby_tank_conversation_phase_t;

typedef struct {
    baby_tank_conversation_phase_t phase;
    baby_tank_expression_t expression;
    uint8_t intensity;
    uint8_t speech_level;
    uint32_t turn_id;
} baby_tank_conversation_snapshot_t;

/* Initializes the provider-independent conversation-to-face adapter. */
esp_err_t baby_tank_conversation_init(void);

/* Safe readiness probe for tasks that may start during boot initialization. */
bool baby_tank_conversation_ready(void);

/* Called by local audio/network state, never inferred from model prose. */
esp_err_t baby_tank_conversation_set_phase(baby_tank_conversation_phase_t phase);

/* Resume listening without replacing the just-finished reply face hold. */
esp_err_t baby_tank_conversation_resume_listening(void);

/* Optional structured metadata from the model; exact whitelist match only. */
esp_err_t baby_tank_conversation_set_ai_emotion(const char *emotion,
                                                uint8_t intensity);

/*
 * Observes the same signed 16-bit mono PCM that is sent to the speaker.
 * This does not own, copy, play, or retain the audio buffer.
 */
esp_err_t baby_tank_conversation_observe_tts_pcm16(const int16_t *samples,
                                                   size_t sample_count);

esp_err_t baby_tank_conversation_get_snapshot(
    baby_tank_conversation_snapshot_t *snapshot);

const char *baby_tank_conversation_phase_name(
    baby_tank_conversation_phase_t phase);
