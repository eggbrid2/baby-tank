#include "device_credentials.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CREDENTIALS_NAMESPACE "baby_tank"
#define CREDENTIALS_WIFI_SSID_KEY "wifi_ssid"
#define CREDENTIALS_WIFI_PASSWORD_KEY "wifi_pass"
#define CREDENTIALS_DEVICE_TOKEN_KEY "device_token"
#define CREDENTIALS_SETUP_POP_KEY "setup_pop"

static const char *TAG = "baby_tank_credentials";

static esp_err_t initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    /* Never erase NVS automatically: it may already contain user credentials. */
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return ESP_ERR_INVALID_STATE;
    }
    return result;
}

static esp_err_t read_string(nvs_handle_t handle, const char *key,
                             char *value, size_t capacity, bool allow_empty)
{
    size_t required = capacity;
    esp_err_t result = nvs_get_str(handle, key, value, &required);
    if (result != ESP_OK) return result;
    return required <= capacity && (allow_empty || required > 1U)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

esp_err_t baby_tank_device_credentials_load(
    baby_tank_device_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(credentials != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "credentials output is required");
    memset(credentials, 0, sizeof(*credentials));
    ESP_RETURN_ON_ERROR(baby_tank_device_identity_load(credentials), TAG,
                        "device identity is unavailable");
    return baby_tank_wifi_credentials_load(credentials);
}

esp_err_t baby_tank_device_identity_load(
    baby_tank_device_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(credentials != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "credentials output is required");
    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "NVS initialization failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(
        nvs_open(CREDENTIALS_NAMESPACE, NVS_READONLY, &handle), TAG,
        "credential namespace is unavailable");
    credentials->device_token[0] = '\0';
    credentials->setup_pop[0] = '\0';
    esp_err_t result = read_string(handle, CREDENTIALS_DEVICE_TOKEN_KEY,
                                   credentials->device_token,
                                   sizeof(credentials->device_token), false);
    if (result == ESP_OK) {
        esp_err_t pop_result = read_string(handle, CREDENTIALS_SETUP_POP_KEY,
                                           credentials->setup_pop,
                                           sizeof(credentials->setup_pop),
                                           false);
        if (pop_result == ESP_ERR_NVS_NOT_FOUND) {
            credentials->setup_pop[0] = '\0';
        } else if (pop_result != ESP_OK) {
            result = pop_result;
        }
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        credentials->device_token[0] = '\0';
        credentials->setup_pop[0] = '\0';
    }
    return result;
}

esp_err_t baby_tank_device_setup_pop_load(char *setup_pop, size_t capacity)
{
    ESP_RETURN_ON_FALSE(setup_pop != NULL && capacity > 1U,
                        ESP_ERR_INVALID_ARG, TAG,
                        "setup code output is required");
    setup_pop[0] = '\0';
    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "NVS initialization failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(
        nvs_open(CREDENTIALS_NAMESPACE, NVS_READONLY, &handle), TAG,
        "credential namespace is unavailable");
    esp_err_t result = read_string(handle, CREDENTIALS_SETUP_POP_KEY,
                                   setup_pop, capacity, false);
    nvs_close(handle);
    if (result != ESP_OK) setup_pop[0] = '\0';
    return result;
}

esp_err_t baby_tank_wifi_credentials_load(
    baby_tank_device_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(credentials != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "credentials output is required");
    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "NVS initialization failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(
        nvs_open(CREDENTIALS_NAMESPACE, NVS_READONLY, &handle), TAG,
        "credential namespace is unavailable");
    credentials->wifi_ssid[0] = '\0';
    credentials->wifi_password[0] = '\0';
    esp_err_t result = read_string(handle, CREDENTIALS_WIFI_SSID_KEY,
                                   credentials->wifi_ssid,
                                   sizeof(credentials->wifi_ssid), false);
    if (result == ESP_OK) {
        result = read_string(handle, CREDENTIALS_WIFI_PASSWORD_KEY,
                             credentials->wifi_password,
                             sizeof(credentials->wifi_password), true);
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        credentials->wifi_ssid[0] = '\0';
        credentials->wifi_password[0] = '\0';
    }
    return result;
}

esp_err_t baby_tank_wifi_credentials_store(const char *ssid,
                                           const char *password)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0' &&
                            strnlen(ssid, BABY_TANK_WIFI_SSID_CAPACITY) <
                                BABY_TANK_WIFI_SSID_CAPACITY &&
                            password != NULL &&
                            strnlen(password,
                                    BABY_TANK_WIFI_PASSWORD_CAPACITY) <
                                BABY_TANK_WIFI_PASSWORD_CAPACITY,
                        ESP_ERR_INVALID_ARG, TAG, "invalid Wi-Fi credentials");
    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "NVS initialization failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(
        nvs_open(CREDENTIALS_NAMESPACE, NVS_READWRITE, &handle), TAG,
        "unable to open credential namespace");
    esp_err_t result = nvs_set_str(handle, CREDENTIALS_WIFI_SSID_KEY, ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, CREDENTIALS_WIFI_PASSWORD_KEY, password);
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t baby_tank_device_credentials_store(
    const baby_tank_device_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(credentials != NULL &&
                            credentials->wifi_ssid[0] != '\0' &&
                            strnlen(credentials->device_token,
                                    sizeof(credentials->device_token)) >= 24U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid credentials");
    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "NVS initialization failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(
        nvs_open(CREDENTIALS_NAMESPACE, NVS_READWRITE, &handle), TAG,
        "unable to open credential namespace");
    esp_err_t result = nvs_set_str(handle, CREDENTIALS_WIFI_SSID_KEY,
                                   credentials->wifi_ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, CREDENTIALS_WIFI_PASSWORD_KEY,
                             credentials->wifi_password);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, CREDENTIALS_DEVICE_TOKEN_KEY,
                             credentials->device_token);
    }
    if (result == ESP_OK && credentials->setup_pop[0] != '\0') {
        result = nvs_set_str(handle, CREDENTIALS_SETUP_POP_KEY,
                             credentials->setup_pop);
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}
