#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t baby_tank_chassis_bridge_start(void);
bool baby_tank_chassis_bridge_ready(void);
bool baby_tank_chassis_bridge_link_verified(void);
uint32_t baby_tank_chassis_bridge_control_epoch(void);
esp_err_t baby_tank_chassis_bridge_drive(int16_t left_per_mille,
                                         int16_t right_per_mille);
esp_err_t baby_tank_chassis_bridge_stop(void);
/* Requires the Baby Tank chassis extension. Stock echo-only firmware returns
 * ESP_ERR_NOT_SUPPORTED and must not be presented as a real percentage. */
esp_err_t baby_tank_chassis_bridge_get_battery_percent(uint8_t *percent);
/* Installed chassis image supports only preset WS2812 modes: 2 white,
 * 6 rainbow, 8 sleep. Mode 8 is not guaranteed to turn off instantly. */
esp_err_t baby_tank_chassis_bridge_light_mode(uint8_t mode);
/* One bounded AI movement. A BLE drive/stop preempts the pulse. */
esp_err_t baby_tank_chassis_bridge_pulse(int16_t left_per_mille,
                                         int16_t right_per_mille,
                                         uint16_t duration_ms);
/* AI/autonomous pulse that is rejected if another controller changed the
 * chassis epoch after the plan was approved. */
esp_err_t baby_tank_chassis_bridge_pulse_guarded(int16_t left_per_mille,
                                                 int16_t right_per_mille,
                                                 uint16_t duration_ms,
                                                 uint32_t expected_epoch);
