#include "baby_tank_ota.h"

#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

static const char *TAG = "baby_tank_ota";

esp_err_t baby_tank_ota_install(const baby_tank_ota_release_t *release)
{
    if (!release || !release->url || !release->device_id || !release->device_token || !release->version) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (release->secure_version < (int)running->secure_version) {
        ESP_LOGE(TAG, "refusing secure-version downgrade: %d < %u",
                 release->secure_version, running->secure_version);
        return ESP_ERR_INVALID_VERSION;
    }

    esp_http_client_config_t http = {
        .url = release->url,
        .username = release->device_id,
        .password = release->device_token,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota = {
        .http_config = &http,
        .partial_http_download = true,
        .max_http_request_size = 64 * 1024,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_app_desc_t next = {0};
    err = esp_https_ota_get_img_desc(handle, &next);
    if (err != ESP_OK || next.secure_version != (uint32_t)release->secure_version ||
        next.secure_version < running->secure_version || strncmp(next.version, release->version, sizeof(next.version)) != 0) {
        ESP_LOGE(TAG, "manifest/image mismatch or forbidden downgrade");
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_VERSION;
    }

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        ESP_LOGD(TAG, "received %d bytes", esp_https_ota_get_image_len_read(handle));
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA verification failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA staged; rebooting into pending-verify image");
    esp_restart();
    return ESP_OK;
}

esp_err_t baby_tank_ota_confirm_current(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return err;
}

void baby_tank_ota_rollback_and_reboot(void)
{
    esp_ota_mark_app_invalid_rollback_and_reboot();
    abort();
}
