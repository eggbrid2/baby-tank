#pragma once

#include "esp_err.h"

/* Starts a USB-only bring-up console. It accepts fixed, low-speed motor test
 * commands and never accepts arbitrary speed or duration values. */
esp_err_t baby_tank_chassis_debug_console_start(void);
