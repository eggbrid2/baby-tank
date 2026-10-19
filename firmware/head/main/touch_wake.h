#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BABY_TANK_TOUCH_TOP = 0,
    BABY_TANK_TOUCH_LEFT,
    BABY_TANK_TOUCH_RIGHT,
} baby_tank_touch_zone_t;

typedef void (*baby_tank_touch_wake_callback_t)(
    baby_tank_touch_zone_t zone,
    void *context);

typedef struct {
    uint32_t trigger_count[3];
    uint16_t delta_per_mille[3];
    uint16_t threshold_per_mille[3];
    uint8_t active_mask;
} baby_tank_touch_snapshot_t;

/*
 * Starts the top, left and right capacitive touch pads used as local inputs.
 * The callback runs in a FreeRTOS task, never in the touch interrupt context.
 */
esp_err_t baby_tank_touch_wake_start(
    baby_tank_touch_wake_callback_t callback,
    void *context);

bool baby_tank_touch_wake_ready(void);
esp_err_t baby_tank_touch_wake_get_snapshot(
    baby_tank_touch_snapshot_t *snapshot);
