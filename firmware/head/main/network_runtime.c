#include "network_runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "cloud_client.h"
#include "chassis_bridge.h"
#include "camera_stream.h"
#include "control_protocol.h"
#include "device_credentials.h"
#include "diagnostics.h"
#include "face_controller.h"
#include "voice_runtime.h"
#include "wifi_portal.h"

#define NETWORK_CONNECTED_BIT BIT0
#define NETWORK_START_TIMEOUT_MS 20000U
#define PROVISIONING_SERVICE_NAME_CAPACITY 16U
#define SOFTAP_SSID_CAPACITY 24U
#define SOFTAP_PASSWORD_CAPACITY 17U
#define SOFTAP_CHANNEL 6U

static const char *TAG = "baby_tank_network";
static EventGroupHandle_t s_events;
static bool s_cloud_ready;
static bool s_provisioning_active;
static bool s_manager_initialized;
static bool s_softap_ready;
static bool s_camera_hotspot_requested;
static bool s_wifi_sleep_power_save;
static volatile baby_tank_web_wifi_phase_t s_web_wifi_phase;
static volatile int s_last_wifi_reason;
static volatile int s_web_failure_reason;
static bool s_web_wifi_busy;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_probe_retry_timer;
static volatile baby_tank_internet_phase_t s_internet_phase;
static volatile int s_internet_http_status;
static bool s_probe_busy;
static wifi_config_t s_softap_config;
static char s_setup_pop[BABY_TANK_SETUP_POP_CAPACITY];
static char s_pending_ssid[BABY_TANK_WIFI_SSID_CAPACITY];
static char s_pending_password[BABY_TANK_WIFI_PASSWORD_CAPACITY];
typedef struct {
    char ssid[BABY_TANK_WIFI_SSID_CAPACITY];
    char password[BABY_TANK_WIFI_PASSWORD_CAPACITY];
} web_wifi_request_t;
static uint8_t s_provisioning_service_uuid[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
};

esp_err_t baby_tank_network_set_sleep_power_save(bool sleeping)
{
    wifi_ps_type_t mode = sleeping ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
    esp_err_t result = esp_wifi_set_ps(mode);
    if (result == ESP_OK && s_wifi_sleep_power_save != sleeping) {
        s_wifi_sleep_power_save = sleeping;
        ESP_LOGI(TAG, "Wi-Fi power save=%s presence=%s",
                 sleeping ? "min-modem" : "off",
                 sleeping ? "sleeping" : "awake");
    }
    return result;
}

static esp_err_t configure_softap(const char *setup_pop)
{
    ESP_RETURN_ON_FALSE(setup_pop != NULL && setup_pop[0] != '\0',
                        ESP_ERR_NOT_FOUND, TAG,
                        "device setup code is required for secure SoftAP");
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), TAG,
                        "unable to read SoftAP identity");
    char ssid[SOFTAP_SSID_CAPACITY];
    int ssid_length = snprintf(ssid, sizeof(ssid), "BabyTank-%02X%02X%02X",
                               mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_FALSE(ssid_length > 0 &&
                            (size_t)ssid_length < sizeof(ssid),
                        ESP_ERR_INVALID_SIZE, TAG, "SoftAP SSID overflow");

    static const char prefix[] = "baby-tank-softap-v1:";
    uint8_t digest[32];
    size_t digest_length = 0;
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    psa_status_t hash_result = psa_crypto_init();
    if (hash_result == PSA_SUCCESS) {
        hash_result = psa_hash_setup(&sha, PSA_ALG_SHA_256);
    }
    if (hash_result == PSA_SUCCESS) {
        hash_result = psa_hash_update(&sha, (const uint8_t *)prefix,
                                      strlen(prefix));
    }
    if (hash_result == PSA_SUCCESS) {
        hash_result = psa_hash_update(
            &sha, (const uint8_t *)setup_pop,
            strlen(setup_pop));
    }
    if (hash_result == PSA_SUCCESS) {
        hash_result = psa_hash_finish(&sha, digest, sizeof(digest),
                                      &digest_length);
    } else {
        psa_hash_abort(&sha);
    }
    ESP_RETURN_ON_FALSE(hash_result == PSA_SUCCESS &&
                            digest_length == sizeof(digest),
                        ESP_FAIL, TAG,
                        "SoftAP password derivation failed");

    char password[SOFTAP_PASSWORD_CAPACITY];
    for (size_t index = 0; index < 8; ++index) {
        snprintf(password + index * 2, 3, "%02x", digest[index]);
    }
    memset(digest, 0, sizeof(digest));
    memset(&s_softap_config, 0, sizeof(s_softap_config));
    memcpy(s_softap_config.ap.ssid, ssid, (size_t)ssid_length);
    s_softap_config.ap.ssid_len = (uint8_t)ssid_length;
    memcpy(s_softap_config.ap.password, password, sizeof(password));
    memset(password, 0, sizeof(password));
    s_softap_config.ap.channel = SOFTAP_CHANNEL;
    s_softap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    s_softap_config.ap.max_connection = 1;
    s_softap_config.ap.pmf_cfg.capable = true;
    /* Keep PMF available without excluding older Android Wi-Fi chipsets. */
    s_softap_config.ap.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                        "SoftAP mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &s_softap_config), TAG,
                        "SoftAP configuration failed");
    s_softap_ready = true;
    ESP_LOGI(TAG, "secure local camera network ready: %s", ssid);
    return ESP_OK;
}

esp_err_t baby_tank_network_enable_camera_hotspot(void)
{
    ESP_RETURN_ON_FALSE(s_setup_pop[0] != '\0', ESP_ERR_INVALID_STATE, TAG,
                        "secure setup code is unavailable");
    s_camera_hotspot_requested = true;
    esp_err_t result = ESP_OK;
    if (!s_softap_ready) {
        result = configure_softap(s_setup_pop);
        if (result == ESP_OK) result = baby_tank_wifi_portal_restart();
    }
    if (result == ESP_OK && !baby_tank_camera_stream_is_ready()) {
        ESP_RETURN_ON_FALSE(baby_tank_voice_runtime_remote_mode(),
                            ESP_ERR_INVALID_STATE, TAG,
                            "camera requires remote mode memory handoff");
        result = baby_tank_camera_stream_start();
    }
    if (result != ESP_OK) s_camera_hotspot_requested = false;
    return result;
}

static esp_err_t control_endpoint(uint32_t session_id, const uint8_t *input,
                                  ssize_t input_size, uint8_t **output,
                                  ssize_t *output_size, void *context)
{
    (void)session_id;
    (void)context;
    ESP_RETURN_ON_FALSE(output != NULL && output_size != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "control output is required");
    baby_tank_control_request_t request = {0};
    bool valid = input_size >= 0 &&
                 baby_tank_control_parse(input, (size_t)input_size, &request);
    baby_tank_control_status_t status = BABY_TANK_CONTROL_INVALID;
    if (valid) (void)baby_tank_face_note_activity();
    if (valid && request.opcode == BABY_TANK_CONTROL_PING) {
        status = BABY_TANK_CONTROL_OK;
    } else if (valid && request.opcode == BABY_TANK_CONTROL_STOP) {
        status = baby_tank_chassis_bridge_stop() == ESP_OK
                     ? BABY_TANK_CONTROL_OK : BABY_TANK_CONTROL_UNAVAILABLE;
        ESP_LOGI(TAG, "BLE stop sequence=%u status=%u",
                 request.sequence, status);
    } else if (valid && request.opcode == BABY_TANK_CONTROL_DRIVE) {
        status = baby_tank_chassis_bridge_drive(request.left_per_mille,
                                                 request.right_per_mille) == ESP_OK
                     ? BABY_TANK_CONTROL_OK : BABY_TANK_CONTROL_UNAVAILABLE;
        ESP_LOGI(TAG, "BLE drive sequence=%u left=%d right=%d status=%u",
                 request.sequence, request.left_per_mille,
                 request.right_per_mille, status);
    } else if (valid && request.opcode == BABY_TANK_CONTROL_LIGHTS &&
               request.right_per_mille == 0) {
        status = baby_tank_chassis_bridge_light_mode(
                     (uint8_t)request.left_per_mille) == ESP_OK
                     ? BABY_TANK_CONTROL_OK : BABY_TANK_CONTROL_UNAVAILABLE;
        ESP_LOGI(TAG, "BLE light sequence=%u mode=%d status=%u",
                 request.sequence, request.left_per_mille, status);
    } else if (valid) {
        status = BABY_TANK_CONTROL_UNAVAILABLE;
    }
    uint8_t *response = malloc(BABY_TANK_CONTROL_PACKET_SIZE);
    ESP_RETURN_ON_FALSE(response != NULL, ESP_ERR_NO_MEM, TAG,
                        "control response allocation failed");
    baby_tank_control_response(response, valid ? &request : NULL, status);
    *output = response;
    *output_size = BABY_TANK_CONTROL_PACKET_SIZE;
    if (valid && status == BABY_TANK_CONTROL_UNAVAILABLE) {
        ESP_LOGW(TAG, "BLE control opcode=%u unavailable",
                 (unsigned)request.opcode);
    }
    return ESP_OK;
}

static esp_err_t personality_endpoint(uint32_t session_id, const uint8_t *input,
                                      ssize_t input_size, uint8_t **output,
                                      ssize_t *output_size, void *context)
{
    (void)session_id;
    (void)context;
    ESP_RETURN_ON_FALSE(output != NULL && output_size != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "personality output required");
    uint8_t *response = malloc(5U);
    ESP_RETURN_ON_FALSE(response != NULL, ESP_ERR_NO_MEM, TAG,
                        "personality response allocation failed");
    response[0] = 1U;
    response[1] = 1U;
    if (input != NULL && input_size == 4 && input[0] == 1U) {
        const baby_tank_personality_t personality = {
            .curiosity = input[1], .warmth = input[2],
            .talkativeness = input[3],
        };
        response[1] = baby_tank_cloud_set_personality(personality) == ESP_OK
                          ? 0U : 1U;
    }
    baby_tank_personality_t current = baby_tank_cloud_get_personality();
    response[2] = current.curiosity;
    response[3] = current.warmth;
    response[4] = current.talkativeness;
    *output = response;
    *output_size = 5U;
    return ESP_OK;
}

/* Called immediately before every Wi-Fi submission. The provisioning manager
 * keeps its previous SUCCESS/FAIL state while BLE remains active for control;
 * without a reset it rejects the next apply request even though set-config
 * reported success. This endpoint is carried inside the Security 1 session. */
static esp_err_t prepare_wifi_endpoint(uint32_t session_id,
                                       const uint8_t *input, ssize_t input_size,
                                       uint8_t **output, ssize_t *output_size,
                                       void *context)
{
    (void)session_id;
    (void)context;
    ESP_RETURN_ON_FALSE(input != NULL && input_size == 1 && input[0] == 1 &&
                            output != NULL && output_size != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid Wi-Fi prepare request");
    /* Reprovisioning disconnects the station. Do not race that deliberate
     * disconnect with our ordinary background reconnect handler. */
    bool was_provisioning = s_provisioning_active;
    s_provisioning_active = true;
    /* A reconnect may already be in progress. The manager resets its state
     * by calling esp_wifi_set_config(), which fails while STA is connecting.
     * Cancel that in-flight attempt before touching the manager state. */
    esp_err_t disconnect_result = esp_wifi_disconnect();
    if (disconnect_result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
    } else {
        ESP_LOGW(TAG, "Wi-Fi disconnect before reprovision: %s",
                 esp_err_to_name(disconnect_result));
    }
    esp_err_t result = network_prov_mgr_reset_wifi_sm_state_on_failure();
    if (result == ESP_ERR_INVALID_STATE) {
        result = network_prov_mgr_reset_wifi_sm_state_for_reprovision();
    }
    if (result != ESP_OK) s_provisioning_active = was_provisioning;
    uint8_t *response = malloc(1);
    ESP_RETURN_ON_FALSE(response != NULL, ESP_ERR_NO_MEM, TAG,
                        "Wi-Fi prepare response allocation failed");
    response[0] = result == ESP_OK ? 0 : 1;
    *output = response;
    *output_size = 1;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "unable to reset Wi-Fi provisioning state: %s",
                 esp_err_to_name(result));
    }
    return ESP_OK;
}

/* This endpoint is available only inside the authenticated BLE provisioning
 * session. The 16-byte camera token is regenerated on each head boot. */
static esp_err_t camera_info_endpoint(uint32_t session_id,
                                      const uint8_t *input, ssize_t input_size,
                                      uint8_t **output, ssize_t *output_size,
                                      void *context)
{
    (void)session_id;
    (void)context;
    ESP_RETURN_ON_FALSE(input != NULL && input_size == 1 &&
                            (input[0] == 1 || input[0] == 2) &&
                            output != NULL && output_size != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid camera info request");
    uint8_t *response = calloc(1, 22);
    ESP_RETURN_ON_FALSE(response != NULL, ESP_ERR_NO_MEM, TAG,
                        "camera info allocation failed");
    response[0] = 1;
    if (input[0] == 2 && baby_tank_network_enable_camera_hotspot() != ESP_OK) {
        response[1] = 2;
        *output = response;
        *output_size = 22;
        return ESP_OK;
    }
    response[1] = baby_tank_network_is_connected() &&
                  baby_tank_camera_stream_lan_info(response + 2,
                                                    response + 6) == ESP_OK
                      ? 0 : 1;
    *output = response;
    *output_size = 22;
    return ESP_OK;
}

static void clear_pending_wifi(void)
{
    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    memset(s_pending_password, 0, sizeof(s_pending_password));
}

static void retry_saved_wifi(void *argument)
{
    (void)argument;
    if (!s_provisioning_active && !baby_tank_network_is_connected()) {
        esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "saved Wi-Fi retry failed: %s",
                     esp_err_to_name(result));
        }
    }
}

typedef struct {
    char body[96];
    size_t length;
} internet_probe_response_t;

static esp_err_t internet_probe_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == NULL ||
        event->data == NULL || event->data_len <= 0) return ESP_OK;
    internet_probe_response_t *response = event->user_data;
    size_t room = sizeof(response->body) - response->length - 1;
    size_t count = (size_t)event->data_len < room ? (size_t)event->data_len : room;
    memcpy(response->body + response->length, event->data, count);
    response->length += count;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static int probe_plain_http_transport(esp_err_t *request_result)
{
    internet_probe_response_t response = {0};
    const esp_http_client_config_t config = {
        .url = "http://www.xydocshare.com/healthz",
        .timeout_ms = 5000,
        .disable_auto_redirect = true,
        .event_handler = internet_probe_event,
        .user_data = &response,
        .buffer_size = 256,
        .buffer_size_tx = 256,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        *request_result = ESP_ERR_NO_MEM;
        return 0;
    }
    *request_result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return status;
}

static void internet_probe_worker(void *argument)
{
    (void)argument;
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station != NULL) {
        for (esp_netif_dns_type_t type = ESP_NETIF_DNS_MAIN;
             type <= ESP_NETIF_DNS_FALLBACK; ++type) {
            esp_netif_dns_info_t dns = {0};
            if (esp_netif_get_dns_info(station, type, &dns) == ESP_OK &&
                dns.ip.type == ESP_IPADDR_TYPE_V4) {
                ESP_LOGI(TAG, "station DNS %d: " IPSTR, (int)type,
                         IP2STR(&dns.ip.u_addr.ip4));
            }
        }
    }
    internet_probe_response_t response = {0};
    const esp_http_client_config_t config = {
        .url = BABY_TANK_CLOUD_ORIGIN "/healthz",
        .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2,
        .event_handler = internet_probe_event,
        .user_data = &response,
        .buffer_size = 256,
        .buffer_size_tx = 256,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t result = client == NULL ? ESP_ERR_NO_MEM :
                       esp_http_client_perform(client);
    int status = client == NULL ? 0 : esp_http_client_get_status_code(client);
    if (client != NULL) esp_http_client_cleanup(client);
    s_internet_http_status = status;
    if (!baby_tank_network_is_connected()) {
        s_internet_phase = BABY_TANK_INTERNET_OFFLINE;
    } else if (result == ESP_OK && status == 200 &&
               strstr(response.body, "\"service\":\"baby-tank-cloud\"") != NULL) {
        s_internet_phase = BABY_TANK_INTERNET_ONLINE;
        ESP_LOGI(TAG, "Baby Tank cloud health probe passed");
    } else if (result == ESP_OK && status >= 300 && status < 400) {
        s_internet_phase = BABY_TANK_INTERNET_PORTAL;
        ESP_LOGW(TAG, "internet probe intercepted; possible captive portal: HTTP %d",
                 status);
    } else {
        esp_err_t plain_result = ESP_FAIL;
        int plain_status = probe_plain_http_transport(&plain_result);
        if (plain_result == ESP_OK && plain_status == 308) {
            s_internet_phase = BABY_TANK_INTERNET_OFFLINE;
            ESP_LOGW(TAG,
                     "cloud HTTP reached but TLS was reset before the server; "
                     "the access network is filtering this device");
        } else if (plain_result == ESP_OK) {
            s_internet_phase = BABY_TANK_INTERNET_PORTAL;
            ESP_LOGW(TAG,
                     "captive portal detected: cloud TLS=%s HTTP=%d, "
                     "plain probe HTTP=%d",
                     esp_err_to_name(result), status, plain_status);
        } else {
            s_internet_phase = BABY_TANK_INTERNET_OFFLINE;
            ESP_LOGW(TAG,
                     "robot internet probe failed: TLS=%s HTTP=%d, "
                     "plain=%s",
                     esp_err_to_name(result), status,
                     esp_err_to_name(plain_result));
        }
    }
    if (s_internet_phase == BABY_TANK_INTERNET_ONLINE) {
        /* Let the setup page display the result before closing the AP. */
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (s_internet_phase == BABY_TANK_INTERNET_ONLINE &&
            baby_tank_network_is_connected() && !s_web_wifi_busy &&
            !s_camera_hotspot_requested) {
            esp_err_t mode_result = esp_wifi_set_mode(WIFI_MODE_STA);
            if (mode_result == ESP_OK) {
                s_softap_ready = false;
                ESP_LOGI(TAG, "internet verified; setup hotspot stopped");
            } else {
                ESP_LOGW(TAG, "unable to stop setup hotspot: %s",
                         esp_err_to_name(mode_result));
            }
        }
    }
    s_probe_busy = false;
    if (s_internet_phase != BABY_TANK_INTERNET_ONLINE &&
        baby_tank_network_is_connected() && s_probe_retry_timer != NULL &&
        !esp_timer_is_active(s_probe_retry_timer)) {
        esp_timer_start_once(s_probe_retry_timer, 30000000);
        ESP_LOGI(TAG, "cloud health probe will retry in 30 s");
    }
    vTaskDelete(NULL);
}

static void internet_probe_retry(void *argument)
{
    (void)argument;
    if (baby_tank_network_is_connected() && !s_probe_busy) {
        baby_tank_network_test_internet_async();
    }
}

esp_err_t baby_tank_network_test_internet_async(void)
{
    ESP_RETURN_ON_FALSE(baby_tank_network_is_connected() && !s_probe_busy,
                        ESP_ERR_INVALID_STATE, TAG,
                        "internet test requires connected station");
    s_probe_busy = true;
    s_internet_phase = BABY_TANK_INTERNET_TESTING;
    s_internet_http_status = 0;
    if (xTaskCreate(internet_probe_worker, "internet_probe", 6144, NULL, 4,
                    NULL) != pdPASS) {
        s_probe_busy = false;
        s_internet_phase = BABY_TANK_INTERNET_OFFLINE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

baby_tank_internet_phase_t baby_tank_network_internet_phase(void)
{
    return s_internet_phase;
}

int baby_tank_network_internet_http_status(void)
{
    return s_internet_http_status;
}

static void network_event(void *argument, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)argument;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_provisioning_active) esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_last_wifi_reason = 0;
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_probe_retry_timer != NULL &&
            esp_timer_is_active(s_probe_retry_timer)) {
            esp_timer_stop(s_probe_retry_timer);
        }
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        if (disconnected != NULL) s_last_wifi_reason = disconnected->reason;
        xEventGroupClearBits(s_events, NETWORK_CONNECTED_BIT);
        s_internet_phase = BABY_TANK_INTERNET_OFFLINE;
        if (!s_softap_ready) {
            char setup_code[BABY_TANK_SETUP_POP_CAPACITY] = {0};
            if (baby_tank_device_setup_pop_load(setup_code,
                                                sizeof(setup_code)) == ESP_OK) {
                esp_err_t ap_result = configure_softap(setup_code);
                if (ap_result != ESP_OK) {
                    ESP_LOGE(TAG, "unable to restore setup hotspot: %s",
                             esp_err_to_name(ap_result));
                }
            }
            memset(setup_code, 0, sizeof(setup_code));
        }
        if (!s_provisioning_active) {
            if (s_reconnect_timer != NULL &&
                esp_timer_is_active(s_reconnect_timer)) {
                esp_timer_stop(s_reconnect_timer);
            }
            if (s_reconnect_timer != NULL) {
                esp_timer_start_once(s_reconnect_timer, 30000000);
            }
            ESP_LOGW(TAG, "Wi-Fi disconnected; retrying saved network in 30 s");
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (s_probe_retry_timer != NULL &&
            esp_timer_is_active(s_probe_retry_timer)) {
            esp_timer_stop(s_probe_retry_timer);
        }
        s_last_wifi_reason = 0;
        xEventGroupSetBits(s_events, NETWORK_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
        if (!s_probe_busy) baby_tank_network_test_internet_async();
    } else if (base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "secure BLE provisioning started");
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            /* BLE provisioning switches APSTA back to STA while applying the
             * home network. Restore the camera AP after validation finishes. */
            s_softap_ready = false;
            const wifi_sta_config_t *wifi = event_data;
            clear_pending_wifi();
            if (wifi != NULL) {
                size_t ssid_length = strnlen((const char *)wifi->ssid,
                                             sizeof(wifi->ssid));
                size_t password_length = strnlen(
                    (const char *)wifi->password, sizeof(wifi->password));
                memcpy(s_pending_ssid, wifi->ssid, ssid_length);
                memcpy(s_pending_password, wifi->password, password_length);
            }
            ESP_LOGI(TAG, "received encrypted Wi-Fi credentials");
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL:
            clear_pending_wifi();
            ESP_LOGW(TAG, "provisioning network validation failed");
            if (configure_softap(s_setup_pop) != ESP_OK) {
                ESP_LOGE(TAG, "unable to restore camera hotspot after Wi-Fi failure");
            }
            break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            s_provisioning_active = false;
            if (s_pending_ssid[0] != '\0') {
                esp_err_t store_result = baby_tank_wifi_credentials_store(
                    s_pending_ssid, s_pending_password);
                if (store_result != ESP_OK) {
                    ESP_LOGW(TAG, "unable to mirror Wi-Fi state: %s",
                             esp_err_to_name(store_result));
                }
            }
            clear_pending_wifi();
            if (configure_softap(s_setup_pop) != ESP_OK) {
                ESP_LOGE(TAG, "unable to restore camera hotspot after Wi-Fi success");
            }
            ESP_LOGI(TAG, "provisioning completed and network verified");
            break;
        case NETWORK_PROV_END:
            s_provisioning_active = false;
            memset(s_setup_pop, 0, sizeof(s_setup_pop));
            if (s_manager_initialized) {
                esp_err_t result = network_prov_mgr_deinit();
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "provisioning cleanup failed: %s",
                             esp_err_to_name(result));
                }
                s_manager_initialized = false;
            }
            break;
        default:
            break;
        }
    }
}

static esp_err_t initialize_platform_network(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "network stack init failed");
    esp_err_t result = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(result == ESP_OK || result == ESP_ERR_INVALID_STATE,
                        result, TAG, "event loop init failed");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL,
                        ESP_ERR_NO_MEM, TAG, "unable to create Wi-Fi station");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap() != NULL,
                        ESP_ERR_NO_MEM, TAG, "unable to create Wi-Fi SoftAP");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "Wi-Fi storage selection failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   network_event, NULL), TAG,
        "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   network_event, NULL), TAG,
        "IP event registration failed");
    return esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                      network_event, NULL);
}

static esp_err_t migrate_legacy_wifi_if_present(void)
{
    baby_tank_device_credentials_t credentials = {0};
    esp_err_t result = baby_tank_wifi_credentials_load(&credentials);
    if (result == ESP_ERR_NVS_NOT_FOUND || result == ESP_ERR_INVALID_SIZE) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, TAG, "legacy Wi-Fi load failed");

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, credentials.wifi_ssid,
            sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, credentials.wifi_password,
            sizeof(config.sta.password));
    config.sta.threshold.authmode = credentials.wifi_password[0] == '\0'
                                        ? WIFI_AUTH_OPEN
                                        : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    memset(&credentials, 0, sizeof(credentials));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(s_softap_ready ? WIFI_MODE_APSTA
                                                        : WIFI_MODE_STA), TAG,
                        "legacy Wi-Fi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "legacy Wi-Fi migration failed");
    memset(&config, 0, sizeof(config));
    ESP_LOGI(TAG, "migrated existing Wi-Fi settings to IDF provisioning store");
    return ESP_OK;
}

static void web_wifi_worker(void *argument)
{
    web_wifi_request_t *request = argument;
    s_provisioning_active = true;
    s_web_wifi_phase = BABY_TANK_WEB_WIFI_CONNECTING;
    s_last_wifi_reason = 0;
    if (s_reconnect_timer != NULL && esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_stop(s_reconnect_timer);
    }
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(350));
    xEventGroupClearBits(s_events, NETWORK_CONNECTED_BIT);
    s_last_wifi_reason = 0;

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, request->ssid,
            sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, request->password,
            sizeof(config.sta.password));
    config.sta.threshold.authmode = request->password[0] == '\0'
                                        ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK) result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result == ESP_OK) result = esp_wifi_connect();
    if (result == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, NETWORK_CONNECTED_BIT, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(20000));
        if ((bits & NETWORK_CONNECTED_BIT) == 0) result = ESP_ERR_TIMEOUT;
    }
    if (result == ESP_OK) {
        result = baby_tank_wifi_credentials_store(request->ssid,
                                                   request->password);
        if (result == ESP_OK) {
            s_web_wifi_phase = BABY_TANK_WEB_WIFI_GOT_IP;
            ESP_LOGI(TAG, "web Wi-Fi setup connected and saved: %s", request->ssid);
        }
    }
    if (result != ESP_OK) {
        s_web_failure_reason = s_last_wifi_reason;
        ESP_LOGW(TAG, "web Wi-Fi setup failed: %s, reason=%d",
                 esp_err_to_name(result), s_web_failure_reason);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        /* A failed candidate must not replace the last working network. */
        esp_err_t restore = migrate_legacy_wifi_if_present();
        s_web_wifi_phase = BABY_TANK_WEB_WIFI_FAILED;
        if (restore != ESP_OK) {
            ESP_LOGW(TAG, "previous Wi-Fi restore failed: %s",
                     esp_err_to_name(restore));
        }
    }
    memset(&config, 0, sizeof(config));
    memset(request, 0, sizeof(*request));
    free(request);
    s_provisioning_active = false;
    s_web_wifi_busy = false;
    vTaskDelete(NULL);
}

esp_err_t baby_tank_network_configure_wifi_async(const char *ssid,
                                                   const char *password)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && password != NULL && s_events != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid web setup request");
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    ESP_RETURN_ON_FALSE(ssid_length > 0 && ssid_length < BABY_TANK_WIFI_SSID_CAPACITY &&
                            password_length < BABY_TANK_WIFI_PASSWORD_CAPACITY &&
                            (password_length == 0 || password_length >= 8) &&
                            strncmp(ssid, "BabyTank-", 9) != 0 &&
                            !s_web_wifi_busy,
                        ESP_ERR_INVALID_ARG, TAG, "invalid or busy Wi-Fi setup");
    web_wifi_request_t *request = calloc(1, sizeof(*request));
    ESP_RETURN_ON_FALSE(request != NULL, ESP_ERR_NO_MEM, TAG,
                        "Wi-Fi request allocation failed");
    strlcpy(request->ssid, ssid, sizeof(request->ssid));
    strlcpy(request->password, password, sizeof(request->password));
    s_web_wifi_busy = true;
    s_web_wifi_phase = BABY_TANK_WEB_WIFI_CONNECTING;
    s_web_failure_reason = 0;
    if (xTaskCreate(web_wifi_worker, "web_wifi_setup", 4096, request, 5,
                    NULL) != pdPASS) {
        s_web_wifi_busy = false;
        memset(request, 0, sizeof(*request));
        free(request);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

baby_tank_web_wifi_phase_t baby_tank_network_web_wifi_phase(void)
{
    return s_web_wifi_phase;
}

int baby_tank_network_last_wifi_reason(void)
{
    return s_web_wifi_phase == BABY_TANK_WEB_WIFI_FAILED
               ? s_web_failure_reason : s_last_wifi_reason;
}

esp_err_t baby_tank_network_scan_wifi(wifi_ap_record_t *records,
                                      uint16_t *record_count)
{
    ESP_RETURN_ON_FALSE(records != NULL && record_count != NULL &&
                            *record_count > 0 && !s_web_wifi_busy,
                        ESP_ERR_INVALID_ARG, TAG, "Wi-Fi scan unavailable");
    bool was_provisioning = s_provisioning_active;
    s_provisioning_active = true;
    if (s_reconnect_timer != NULL && esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (!baby_tank_network_is_connected()) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    esp_err_t result = esp_wifi_scan_start(NULL, true);
    if (result == ESP_OK) result = esp_wifi_scan_get_ap_records(record_count,
                                                                 records);
    s_provisioning_active = was_provisioning;
    if (!was_provisioning && s_reconnect_timer != NULL &&
        !esp_timer_is_active(s_reconnect_timer) &&
        !baby_tank_network_is_connected()) {
        esp_timer_start_once(s_reconnect_timer, 30000000);
    }
    return result;
}

static esp_err_t start_ble_provisioning(
    const baby_tank_device_credentials_t *identity)
{
    ESP_RETURN_ON_FALSE(identity->setup_pop[0] != '\0', ESP_ERR_NOT_FOUND,
                        TAG, "device setup code is missing from NVS");
    strlcpy(s_setup_pop, identity->setup_pop, sizeof(s_setup_pop));

    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), TAG,
                        "unable to read station MAC");
    char service_name[PROVISIONING_SERVICE_NAME_CAPACITY];
    int written = snprintf(service_name, sizeof(service_name),
                           "BabyTank-%02X%02X%02X",
                           mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < sizeof(service_name),
                        ESP_ERR_INVALID_SIZE, TAG,
                        "provisioning service name overflow");

    ESP_RETURN_ON_ERROR(network_prov_scheme_ble_set_service_uuid(
                            s_provisioning_service_uuid),
                        TAG, "unable to set provisioning UUID");
    const char *capabilities[] = {
        "ble-control-v1",
        "ble-personality-v1",
        "ble-diagnostics-v1",
        "device-volume-v1",
        "safe-stop-required",
        "head-only",
    };
    ESP_RETURN_ON_ERROR(
        network_prov_mgr_set_app_info("baby-tank", "1.0", capabilities,
                                      sizeof(capabilities) /
                                          sizeof(capabilities[0])),
        TAG, "unable to publish device capabilities");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_create("baby-control"), TAG,
                        "unable to create BLE control endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_create("baby-personality"), TAG,
                        "unable to create BLE personality endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_create("baby-wifi-prepare"),
                        TAG, "unable to create Wi-Fi prepare endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_create("baby-camera-info"),
                        TAG, "unable to create camera info endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_create("baby-diagnostics"),
                        TAG, "unable to create diagnostics endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_disable_auto_stop(1000), TAG,
                        "unable to keep BLE control online");
    s_provisioning_active = true;
    esp_err_t result = network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_1,
        (const network_prov_security1_params_t *)s_setup_pop,
        service_name, NULL);
    if (result != ESP_OK) {
        s_provisioning_active = false;
        memset(s_setup_pop, 0, sizeof(s_setup_pop));
        return result;
    }
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(
                            "baby-control", control_endpoint, NULL),
                        TAG, "unable to register BLE control endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(
                            "baby-personality", personality_endpoint, NULL),
                        TAG, "unable to register BLE personality endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(
                            "baby-wifi-prepare", prepare_wifi_endpoint, NULL),
                        TAG, "unable to register Wi-Fi prepare endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(
                            "baby-camera-info", camera_info_endpoint, NULL),
                        TAG, "unable to register camera info endpoint");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(
                            "baby-diagnostics",
                            baby_tank_diagnostics_endpoint, NULL),
                        TAG, "unable to register diagnostics endpoint");
    /* Provisioning starts Wi-Fi in station mode. Restore APSTA afterwards so
     * the camera network and encrypted BLE session coexist. */
    return configure_softap(identity->setup_pop);
}

esp_err_t baby_tank_network_start(void)
{
    ESP_RETURN_ON_FALSE(s_events == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "network runtime already started");
    esp_err_t chassis_result = baby_tank_chassis_bridge_start();
    if (chassis_result != ESP_OK) {
        ESP_LOGW(TAG, "chassis UART bridge unavailable: %s",
                 esp_err_to_name(chassis_result));
    }
    baby_tank_device_credentials_t identity = {0};
    esp_err_t identity_result = baby_tank_device_identity_load(&identity);
    if (identity_result == ESP_OK) {
        esp_err_t cloud_result = baby_tank_cloud_init(identity.device_token);
        if (cloud_result == ESP_OK) {
            s_cloud_ready = true;
        } else {
            ESP_LOGW(TAG, "cloud client unavailable: %s",
                     esp_err_to_name(cloud_result));
        }
    } else {
        ESP_LOGW(TAG, "device identity missing; cloud dialogue disabled");
    }
    esp_err_t setup_result = baby_tank_device_setup_pop_load(
        identity.setup_pop, sizeof(identity.setup_pop));
    if (setup_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "device setup code missing; BLE provisioning and local camera disabled");
    }

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create network event group");
    ESP_RETURN_ON_ERROR(initialize_platform_network(), TAG,
                        "network platform init failed");
    const esp_timer_create_args_t retry_timer_args = {
        .callback = retry_saved_wifi,
        .name = "saved_wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_timer_args, &s_reconnect_timer),
                        TAG, "Wi-Fi retry timer creation failed");
    const esp_timer_create_args_t probe_retry_args = {
        .callback = internet_probe_retry,
        .name = "cloud_probe_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&probe_retry_args, &s_probe_retry_timer),
                        TAG, "cloud probe retry timer creation failed");

    network_prov_mgr_config_t manager_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .network_prov_wifi_conn_cfg = {
            .wifi_conn_attempts = 5,
        },
    };
    ESP_RETURN_ON_ERROR(network_prov_mgr_init(manager_config), TAG,
                        "provisioning manager init failed");
    s_manager_initialized = true;

    ESP_RETURN_ON_ERROR(migrate_legacy_wifi_if_present(), TAG,
                        "legacy Wi-Fi migration failed");
    bool provisioned = false;
    ESP_RETURN_ON_ERROR(network_prov_mgr_is_wifi_provisioned(&provisioned), TAG,
                        "unable to read provisioning state");
    esp_err_t provisioning_result = start_ble_provisioning(&identity);
    if (provisioning_result != ESP_OK) {
        if (provisioned) {
            ESP_LOGW(TAG, "BLE control unavailable: %s",
                     esp_err_to_name(provisioning_result));
            network_prov_mgr_deinit();
            s_manager_initialized = false;
            memset(&identity, 0, sizeof(identity));
            ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                                "Wi-Fi station mode failed");
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG,
                                "Wi-Fi start failed");
            return baby_tank_network_wait_connected(
                NETWORK_START_TIMEOUT_MS);
        }
        memset(&identity, 0, sizeof(identity));
        network_prov_mgr_deinit();
        s_manager_initialized = false;
        return provisioning_result;
    }
    memset(&identity, 0, sizeof(identity));
    if (s_softap_ready) {
        esp_err_t portal_result = baby_tank_wifi_portal_start();
        if (portal_result != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi setup page unavailable: %s",
                     esp_err_to_name(portal_result));
        }
    }
    if (!provisioned) {
        ESP_LOGI(TAG, "advertising secure provisioning service");
        return ESP_ERR_NOT_FINISHED;
    }

    /* Starting provisioning clears the active RAM copy but preserves Flash.
     * Restore our mirrored config so BLE control and Wi-Fi can coexist. */
    ESP_RETURN_ON_ERROR(migrate_legacy_wifi_if_present(), TAG,
                        "unable to restore provisioned Wi-Fi");
    s_provisioning_active = false;
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG,
                        "Wi-Fi connection start failed");
    ESP_LOGI(TAG, "secure BLE control endpoint remains available");
    return baby_tank_network_wait_connected(NETWORK_START_TIMEOUT_MS);
}

esp_err_t baby_tank_network_wait_connected(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "network runtime is not started");
    EventBits_t bits = xEventGroupWaitBits(
        s_events, NETWORK_CONNECTED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & NETWORK_CONNECTED_BIT) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool baby_tank_network_is_connected(void)
{
    return s_events != NULL &&
           (xEventGroupGetBits(s_events) & NETWORK_CONNECTED_BIT) != 0U;
}

bool baby_tank_network_cloud_is_ready(void)
{
    return s_cloud_ready;
}

bool baby_tank_network_softap_is_ready(void)
{
    return s_softap_ready;
}
