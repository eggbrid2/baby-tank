#include "device_credentials.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CREDENTIALS_NAMESPACE "baby_tank"
#define CREDENTIALS_WIFI_SSID_KEY "wifi_ssid"
#define CREDENTIALS_WIFI_PASSWORD_KEY "wifi_pass"
#define CREDENTIALS_WIFI_PROFILES_KEY "wifi_profiles"
#define WIFI_PROFILES_VERSION 1U
#define CREDENTIALS_DEVICE_TOKEN_KEY "device_token"
#define CREDENTIALS_SETUP_POP_KEY "setup_pop"

static const char *TAG = "baby_tank_credentials";
static esp_err_t read_string(nvs_handle_t handle, const char *key,
                             char *value, size_t capacity, bool allow_empty);

typedef struct {
    uint8_t version;
    uint8_t count;
    baby_tank_wifi_profile_t profiles[BABY_TANK_WIFI_PROFILE_MAX];
} stored_wifi_profiles_t;

static bool valid_profiles(const stored_wifi_profiles_t *stored)
{
    if (stored->version != WIFI_PROFILES_VERSION || stored->count == 0U ||
        stored->count > BABY_TANK_WIFI_PROFILE_MAX) return false;
    for (size_t i = 0; i < stored->count; ++i) {
        const baby_tank_wifi_profile_t *profile = &stored->profiles[i];
        if (strnlen(profile->ssid, sizeof(profile->ssid)) == 0U ||
            strnlen(profile->ssid, sizeof(profile->ssid)) >= sizeof(profile->ssid) ||
            strnlen(profile->password, sizeof(profile->password)) >=
                sizeof(profile->password)) return false;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(profile->ssid, stored->profiles[j].ssid) == 0)
                return false;
        }
    }
    return true;
}

static esp_err_t read_profiles(nvs_handle_t handle,
                               stored_wifi_profiles_t *stored)
{
    memset(stored, 0, sizeof(*stored));
    size_t size = sizeof(*stored);
    esp_err_t result = nvs_get_blob(handle, CREDENTIALS_WIFI_PROFILES_KEY,
                                    stored, &size);
    if (result != ESP_OK) return result;
    return size == sizeof(*stored) && valid_profiles(stored)
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t read_legacy_profile(nvs_handle_t handle,
                                     baby_tank_wifi_profile_t *profile)
{
    memset(profile, 0, sizeof(*profile));
    esp_err_t result = read_string(handle, CREDENTIALS_WIFI_SSID_KEY,
                                   profile->ssid, sizeof(profile->ssid), false);
    if (result == ESP_OK) {
        result = read_string(handle, CREDENTIALS_WIFI_PASSWORD_KEY,
                             profile->password, sizeof(profile->password), true);
    }
    if (result != ESP_OK) memset(profile, 0, sizeof(*profile));
    return result;
}

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
    baby_tank_wifi_profile_t profile = {0};
    stored_wifi_profiles_t stored = {0};
    esp_err_t result = read_profiles(handle, &stored);
    if (result == ESP_OK) profile = stored.profiles[0];
    else result = read_legacy_profile(handle, &profile);
    nvs_close(handle);
    memset(&stored, 0, sizeof(stored));
    strlcpy(credentials->wifi_ssid, profile.ssid,
            sizeof(credentials->wifi_ssid));
    strlcpy(credentials->wifi_password, profile.password,
            sizeof(credentials->wifi_password));
    memset(&profile, 0, sizeof(profile));
    return result;
}

esp_err_t baby_tank_wifi_profiles_load(baby_tank_wifi_profile_t *profiles,
                                      size_t capacity, size_t *count)
{
    ESP_RETURN_ON_FALSE(profiles != NULL && count != NULL &&
                            capacity >= BABY_TANK_WIFI_PROFILE_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid profile output");
    *count = 0U;
    memset(profiles, 0, capacity * sizeof(*profiles));
    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "NVS initialization failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(CREDENTIALS_NAMESPACE, NVS_READONLY, &handle),
                        TAG, "credential namespace is unavailable");
    stored_wifi_profiles_t stored = {0};
    esp_err_t result = read_profiles(handle, &stored);
    if (result == ESP_OK) {
        *count = stored.count;
        memcpy(profiles, stored.profiles, stored.count * sizeof(*profiles));
    } else {
        result = read_legacy_profile(handle, &profiles[0]);
        if (result == ESP_OK) *count = 1U;
    }
    nvs_close(handle);
    memset(&stored, 0, sizeof(stored));
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
    stored_wifi_profiles_t previous = {0};
    esp_err_t existing = read_profiles(handle, &previous);
    if (existing != ESP_OK) {
        baby_tank_wifi_profile_t legacy = {0};
        if (read_legacy_profile(handle, &legacy) == ESP_OK) {
            previous.version = WIFI_PROFILES_VERSION;
            previous.count = 1U;
            previous.profiles[0] = legacy;
        }
        memset(&legacy, 0, sizeof(legacy));
    }
    stored_wifi_profiles_t next = {
        .version = WIFI_PROFILES_VERSION, .count = 1U,
    };
    strlcpy(next.profiles[0].ssid, ssid, sizeof(next.profiles[0].ssid));
    strlcpy(next.profiles[0].password, password,
            sizeof(next.profiles[0].password));
    for (size_t i = 0; i < previous.count &&
                       next.count < BABY_TANK_WIFI_PROFILE_MAX; ++i) {
        if (strcmp(previous.profiles[i].ssid, ssid) == 0) continue;
        next.profiles[next.count++] = previous.profiles[i];
    }
    esp_err_t result = nvs_set_str(handle, CREDENTIALS_WIFI_SSID_KEY, ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, CREDENTIALS_WIFI_PASSWORD_KEY, password);
    }
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, CREDENTIALS_WIFI_PROFILES_KEY,
                              &next, sizeof(next));
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    memset(&previous, 0, sizeof(previous));
    memset(&next, 0, sizeof(next));
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
