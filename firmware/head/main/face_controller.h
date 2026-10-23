#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "display.h"

#define BABY_TANK_FACE_DURATION_HOLD UINT32_MAX

typedef enum {
    BABY_TANK_FACE_PRIORITY_IDLE = 0,
    BABY_TANK_FACE_PRIORITY_AMBIENT = 20,
    BABY_TANK_FACE_PRIORITY_AI = 50,
    BABY_TANK_FACE_PRIORITY_INTERACTION = 70,
    BABY_TANK_FACE_PRIORITY_SAFETY = 100,
} baby_tank_face_priority_t;

typedef enum {
    BABY_TANK_PRESENCE_NORMAL = 0,
    BABY_TANK_PRESENCE_BORED,
    BABY_TANK_PRESENCE_SLEEPING,
} baby_tank_presence_state_t;

typedef struct {
    baby_tank_expression_t expression;
    uint32_t duration_ms;
    uint8_t intensity;
    baby_tank_face_priority_t priority;
} baby_tank_face_request_t;

typedef struct {
    baby_tank_expression_t expression;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    uint8_t intensity;
    uint8_t speech_level;
    baby_tank_face_priority_t priority;
} baby_tank_face_snapshot_t;

/* Initializes the display and starts the single owner rendering task. */
esp_err_t baby_tank_face_controller_start(void);

/* Trusted firmware entry point for touch, audio, system, and safety events. */
esp_err_t baby_tank_face_submit(const baby_tank_face_request_t *request);
/* Submitting a visual does not count as user activity. Event sources must call
 * baby_tank_face_note_activity() explicitly so background work cannot wake it. */

/*
 * Restricted AI entry point. Emotion names are matched against the display
 * whitelist; priority is always AI and numeric fields are clamped.
 */
esp_err_t baby_tank_face_submit_ai(const char *emotion,
                                   uint8_t intensity,
                                   uint32_t duration_ms);
/* Long-lived fictional mood shown while awake but idle. */
esp_err_t baby_tank_face_set_mood(const char *emotion, uint8_t intensity);
/* A fresh cloud time band changes only the idle schedule, not the safety gate. */
esp_err_t baby_tank_face_set_time_band(const char *band);
/* Any real interaction returns the pet to NORMAL and restarts its idle clock. */
esp_err_t baby_tank_face_note_activity(void);
/* Explicit sleep/wake override. Ordinary inactivity is handled automatically. */
esp_err_t baby_tank_face_set_standby(bool standby);
esp_err_t baby_tank_face_get_presence_state(
    baby_tank_presence_state_t *state);
/* Local USB-only art check. Never changes the idle timer or persists state;
 * real interaction and the deep-night policy always cancel the preview. */
esp_err_t baby_tank_face_debug_preview_scene(baby_tank_scene_t scene);
void baby_tank_face_debug_stop_preview(void);
/* Cloud supplies only an allowlisted pictogram key, never drawing commands. */
esp_err_t baby_tank_face_set_thought(const char *thought);
esp_err_t baby_tank_face_set_thought_text(const char *utf8_text);
/* Takes ownership of a validated 240x240 RGB565 SVG raster for up to 15 seconds. */
esp_err_t baby_tank_face_submit_drawing(const baby_tank_drawing_t *drawing);
void baby_tank_face_cancel_drawing(void);

/* Exact whitelist lookup for a validated model or protocol adapter. */
esp_err_t baby_tank_face_expression_from_name(const char *emotion,
                                              baby_tank_expression_t *expression);

/* Audio playback envelope, 0..100. It never changes expression priority. */
esp_err_t baby_tank_face_set_speech_level(uint8_t level);

/* Thread-safe diagnostic snapshot for logs, telemetry, or a future web UI. */
esp_err_t baby_tank_face_get_snapshot(baby_tank_face_snapshot_t *snapshot);
