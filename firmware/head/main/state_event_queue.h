#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum {
    BABY_TANK_STATE_EVENT_SIDE_TOUCH = 1,
    BABY_TANK_STATE_EVENT_SCENE_COMPLETED = 2,
} baby_tank_state_event_kind_t;

esp_err_t baby_tank_state_events_init(void);
esp_err_t baby_tank_state_event_enqueue(baby_tank_state_event_kind_t kind);
bool baby_tank_state_event_peek(char id[37],
                               baby_tank_state_event_kind_t *kind);
void baby_tank_state_event_mark_uploaded(const char *id);
esp_err_t baby_tank_state_events_persist_if_dirty(void);
