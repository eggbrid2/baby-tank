#pragma once

#include <stddef.h>

#include "esp_err.h"

#define BABY_TANK_WIFI_SSID_CAPACITY 33U
#define BABY_TANK_WIFI_PASSWORD_CAPACITY 65U
#define BABY_TANK_DEVICE_TOKEN_CAPACITY 192U
#define BABY_TANK_SETUP_POP_CAPACITY 65U

typedef struct {
    char wifi_ssid[BABY_TANK_WIFI_SSID_CAPACITY];
    char wifi_password[BABY_TANK_WIFI_PASSWORD_CAPACITY];
    char device_token[BABY_TANK_DEVICE_TOKEN_CAPACITY];
    char setup_pop[BABY_TANK_SETUP_POP_CAPACITY];
} baby_tank_device_credentials_t;

/* Credentials live only in NVS. They are never supplied through sdkconfig. */
esp_err_t baby_tank_device_credentials_load(
    baby_tank_device_credentials_t *credentials);

/* Cloud identity and Wi-Fi are intentionally readable independently. */
esp_err_t baby_tank_device_identity_load(
    baby_tank_device_credentials_t *credentials);
esp_err_t baby_tank_device_setup_pop_load(char *setup_pop, size_t capacity);
esp_err_t baby_tank_wifi_credentials_load(
    baby_tank_device_credentials_t *credentials);
esp_err_t baby_tank_wifi_credentials_store(
    const char *ssid, const char *password);

/* Provisioning UI may call this later; current firmware never logs the values. */
esp_err_t baby_tank_device_credentials_store(
    const baby_tank_device_credentials_t *credentials);
