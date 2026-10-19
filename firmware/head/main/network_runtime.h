#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

esp_err_t baby_tank_network_start(void);
esp_err_t baby_tank_network_wait_connected(uint32_t timeout_ms);
bool baby_tank_network_is_connected(void);
bool baby_tank_network_cloud_is_ready(void);
bool baby_tank_network_softap_is_ready(void);
esp_err_t baby_tank_network_enable_camera_hotspot(void);
/* Wi-Fi modem sleep is permitted only while the pet is actually sleeping. */
esp_err_t baby_tank_network_set_sleep_power_save(bool sleeping);

typedef enum {
    BABY_TANK_WEB_WIFI_IDLE,
    BABY_TANK_WEB_WIFI_CONNECTING,
    BABY_TANK_WEB_WIFI_GOT_IP,
    BABY_TANK_WEB_WIFI_FAILED,
} baby_tank_web_wifi_phase_t;

esp_err_t baby_tank_network_configure_wifi_async(const char *ssid,
                                                  const char *password);
baby_tank_web_wifi_phase_t baby_tank_network_web_wifi_phase(void);
int baby_tank_network_last_wifi_reason(void);

typedef enum {
    BABY_TANK_INTERNET_UNTESTED,
    BABY_TANK_INTERNET_TESTING,
    BABY_TANK_INTERNET_ONLINE,
    BABY_TANK_INTERNET_PORTAL,
    BABY_TANK_INTERNET_OFFLINE,
} baby_tank_internet_phase_t;

esp_err_t baby_tank_network_test_internet_async(void);
baby_tank_internet_phase_t baby_tank_network_internet_phase(void);
int baby_tank_network_internet_http_status(void);
esp_err_t baby_tank_network_scan_wifi(wifi_ap_record_t *records,
                                     uint16_t *record_count);
