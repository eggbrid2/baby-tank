#pragma once

#include "esp_err.h"

typedef struct {
    const char *url;
    const char *device_id;
    const char *device_token;
    const char *version;
    int secure_version;
} baby_tank_ota_release_t;

/**
 * Download to the inactive OTA slot and reboot on success.
 * The running image is not marked valid here; the next boot must finish its
 * hardware/network self-test and call baby_tank_ota_confirm_current().
 */
esp_err_t baby_tank_ota_install(const baby_tank_ota_release_t *release);

/** Mark a pending-verify image valid after the application self-test passes. */
esp_err_t baby_tank_ota_confirm_current(void);

/** Roll back a pending-verify image immediately after a failed self-test. */
void baby_tank_ota_rollback_and_reboot(void) __attribute__((noreturn));
