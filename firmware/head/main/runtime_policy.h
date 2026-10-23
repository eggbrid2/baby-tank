#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BABY_TANK_POLICY_MAX_WINDOWS 9U

typedef struct {
    int64_t start_epoch_ms;
    int64_t end_epoch_ms;
} baby_tank_policy_window_t;

/* A cold boot without valid time allows interaction by owner choice, even if
 * the saved night lock is enabled. Power-cycling can therefore bypass it. */
esp_err_t baby_tank_policy_init(void);
esp_err_t baby_tank_policy_apply(uint32_t revision, bool enabled,
                                int64_t server_epoch_ms,
                                int64_t valid_until_epoch_ms,
                                const baby_tank_policy_window_t *windows,
                                size_t window_count,
                                const char *time_zone);
bool baby_tank_policy_is_locked(void);
/* True only when a trusted schedule places us at least two hours before the
 * next night window. Used for offline daytime presentation, not access. */
bool baby_tank_policy_is_daytime(void);
bool baby_tank_policy_time_known(void);
bool baby_tank_policy_tick(void); /* true on transition into lock */
uint32_t baby_tank_policy_revision(void);
const char *baby_tank_policy_clock_quality(void);
/* A physically attached maintainer may anchor the verified Shanghai schedule
 * to a trusted host clock. No network or motor command is involved. The
 * persisted anchor is usable only while the ESP RTC has kept running. */
esp_err_t baby_tank_policy_usb_sync_shanghai_clock(uint32_t revision,
                                                   int64_t host_epoch_ms);
