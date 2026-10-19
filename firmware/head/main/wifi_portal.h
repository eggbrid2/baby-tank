#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* The setup page owns the AP-only HTTP server. Camera routes are attached
 * later, so Wi-Fi setup remains available even if the sensor fails. */
esp_err_t baby_tank_wifi_portal_start(void);
esp_err_t baby_tank_wifi_portal_stop(void);
esp_err_t baby_tank_wifi_portal_restart(void);
httpd_handle_t baby_tank_wifi_portal_server(void);
