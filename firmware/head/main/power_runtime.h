#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t baby_tank_power_runtime_start(void);
bool baby_tank_power_head_percent(uint8_t *percent);
bool baby_tank_power_chassis_percent(uint8_t *percent);
