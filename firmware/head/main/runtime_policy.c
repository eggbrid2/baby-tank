#include "runtime_policy.h"

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "policy_clock_math.h"

#define POLICY_STORED_VERSION 3U
#define POLICY_VERIFIED_MS (15LL * 60LL * 1000LL)
#define POLICY_PERSIST_INTERVAL_MS (12LL * 60LL * 60LL * 1000LL)

typedef struct {
    uint8_t version;
    uint8_t enabled;
    uint32_t revision;
} stored_policy_v1_t;

typedef struct {
    uint8_t version;
    uint8_t enabled;
    uint32_t revision;
    int64_t server_epoch_ms;
    int64_t valid_until_epoch_ms;
    uint8_t window_count;
    baby_tank_policy_window_t windows[BABY_TANK_POLICY_MAX_WINDOWS];
} stored_policy_v2_t;

typedef struct {
    stored_policy_v2_t base;
    uint8_t shanghai_daily_rule;
} stored_policy_t;

static const char *TAG = "baby_tank_policy";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* The saved policy is loaded during network initialization. Before that,
 * clock-unknown boot remains interactive by the owner's explicit choice. */
static bool s_enabled = true;
static bool s_last_locked;
static uint32_t s_revision;
static int64_t s_server_epoch_ms;
static int64_t s_synced_us;
static int64_t s_valid_until_epoch_ms;
static baby_tank_policy_window_t s_windows[BABY_TANK_POLICY_MAX_WINDOWS];
static size_t s_window_count;
static int64_t s_last_persisted_epoch_ms;
static bool s_restored_clock;
static bool s_shanghai_daily_rule;
static bool s_usb_clock_source;

static int64_t rtc_epoch_ms(void)
{
    struct timeval now = {0};
    if (gettimeofday(&now, NULL) != 0) return 0;
    return (int64_t)now.tv_sec * 1000LL + now.tv_usec / 1000LL;
}

static int64_t estimated_epoch_ms(void)
{
    return s_server_epoch_ms + (esp_timer_get_time() - s_synced_us) / 1000LL;
}

static bool locked_now(void)
{
    if (!s_enabled) return false;
    /* There is no battery-backed RTC. After a cold boot with no network we
     * cannot distinguish 05:00 from 10:00. The owner chose availability in
     * this case: an unknown clock must not masquerade as nighttime. Once a
     * trusted clock arrives, the normal night gate takes effect immediately. */
    if (s_synced_us == 0) return false;
    int64_t now = estimated_epoch_ms();
    if (s_shanghai_daily_rule) {
        return baby_tank_shanghai_night_locked(now);
    }
    if (now >= s_valid_until_epoch_ms) return true;
    for (size_t index = 0; index < s_window_count; ++index) {
        if (now >= s_windows[index].start_epoch_ms &&
            now < s_windows[index].end_epoch_ms) return true;
    }
    return false;
}

esp_err_t baby_tank_policy_init(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("baby_policy", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        s_enabled = false;
        return ESP_OK;
    }
    if (result != ESP_OK) return result;
    stored_policy_t stored = {0};
    size_t size = sizeof(stored);
    result = nvs_get_blob(handle, "policy", &stored, &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        s_enabled = false;
        return ESP_OK;
    }
    if (result != ESP_OK) return result;
    bool legacy_v1 = size == sizeof(stored_policy_v1_t) &&
                     stored.base.version == 1U;
    bool legacy_v2 = size == sizeof(stored_policy_v2_t) &&
                     stored.base.version == 2U;
    bool current = size == sizeof(stored) &&
                   stored.base.version == POLICY_STORED_VERSION &&
                   stored.shanghai_daily_rule <= 1U;
    if ((!legacy_v1 && !legacy_v2 && !current) ||
        stored.base.enabled > 1U) return ESP_ERR_INVALID_SIZE;
    int64_t rtc_now = rtc_epoch_ms();
    portENTER_CRITICAL(&s_lock);
    s_enabled = stored.base.enabled != 0U;
    s_revision = stored.base.revision;
    s_shanghai_daily_rule = current && stored.shanghai_daily_rule != 0U;
    s_usb_clock_source = s_shanghai_daily_rule &&
        stored.base.window_count == 0U &&
        stored.base.valid_until_epoch_ms == INT64_MAX;
    /* ESP-IDF's RTC-backed gettimeofday survives a software reset with power
     * present. A cold boot without valid time remains explicitly unknown. */
    if (!legacy_v1 &&
        stored.base.window_count <= BABY_TANK_POLICY_MAX_WINDOWS &&
        stored.base.server_epoch_ms >= 1700000000000LL &&
        rtc_now >= stored.base.server_epoch_ms &&
        (s_shanghai_daily_rule ||
         rtc_now < stored.base.valid_until_epoch_ms)) {
        s_server_epoch_ms = rtc_now;
        s_synced_us = esp_timer_get_time();
        s_valid_until_epoch_ms = stored.base.valid_until_epoch_ms;
        s_window_count = stored.base.window_count;
        memcpy(s_windows, stored.base.windows,
               s_window_count * sizeof(*s_windows));
        s_last_persisted_epoch_ms = stored.base.server_epoch_ms;
        s_restored_clock = true;
    }
    s_last_locked = locked_now();
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "restored policy revision=%lu enabled=%d clock=%s locked=%d",
             (unsigned long)stored.base.revision, (int)s_enabled,
             s_restored_clock ? "rtc_estimated" : "unknown",
             (int)s_last_locked);
    return ESP_OK;
}

esp_err_t baby_tank_policy_apply(uint32_t revision, bool enabled,
                                int64_t server_epoch_ms,
                                int64_t valid_until_epoch_ms,
                                const baby_tank_policy_window_t *windows,
                                size_t window_count,
                                const char *time_zone)
{
    if (revision < 1U || server_epoch_ms < 1700000000000LL ||
        valid_until_epoch_ms <= server_epoch_ms ||
        window_count > BABY_TANK_POLICY_MAX_WINDOWS ||
        (window_count > 0U && windows == NULL) ||
        time_zone == NULL) return ESP_ERR_INVALID_ARG;
    bool shanghai_daily_rule = strcmp(time_zone, "Asia/Shanghai") == 0;
    for (size_t index = 0; index < window_count; ++index) {
        if (windows[index].start_epoch_ms >= windows[index].end_epoch_ms ||
            windows[index].end_epoch_ms > valid_until_epoch_ms ||
            (index > 0U && windows[index - 1U].end_epoch_ms >
                windows[index].start_epoch_ms)) return ESP_ERR_INVALID_ARG;
    }
    bool persist;
    portENTER_CRITICAL(&s_lock);
    persist = revision != s_revision || enabled != s_enabled ||
              shanghai_daily_rule != s_shanghai_daily_rule ||
              s_usb_clock_source ||
              s_last_persisted_epoch_ms == 0 ||
              server_epoch_ms - s_last_persisted_epoch_ms >=
                  POLICY_PERSIST_INTERVAL_MS;
    portEXIT_CRITICAL(&s_lock);
    if (persist) {
        nvs_handle_t handle;
        esp_err_t result = nvs_open("baby_policy", NVS_READWRITE, &handle);
        if (result != ESP_OK) return result;
        stored_policy_t stored = { .base = {
            .version = POLICY_STORED_VERSION,
            .enabled = enabled ? 1U : 0U, .revision = revision,
            .server_epoch_ms = server_epoch_ms,
            .valid_until_epoch_ms = valid_until_epoch_ms,
            .window_count = (uint8_t)window_count },
            .shanghai_daily_rule = shanghai_daily_rule ? 1U : 0U };
        if (window_count > 0U) memcpy(stored.base.windows, windows,
                                     window_count * sizeof(*windows));
        result = nvs_set_blob(handle, "policy", &stored, sizeof(stored));
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
        if (result != ESP_OK) return result;
    }
    portENTER_CRITICAL(&s_lock);
    s_enabled = enabled;
    s_revision = revision;
    s_server_epoch_ms = server_epoch_ms;
    s_synced_us = esp_timer_get_time();
    s_restored_clock = false;
    s_shanghai_daily_rule = shanghai_daily_rule;
    s_usb_clock_source = false;
    s_valid_until_epoch_ms = valid_until_epoch_ms;
    s_window_count = window_count;
    if (persist) s_last_persisted_epoch_ms = server_epoch_ms;
    if (window_count > 0U) memcpy(s_windows, windows,
                                  window_count * sizeof(*windows));
    portEXIT_CRITICAL(&s_lock);
    /* Keep the same trusted wall clock across a software reboot. This does
     * not fabricate elapsed time after a power-off: that case stays locked. */
    struct timeval wall = { .tv_sec = (time_t)(server_epoch_ms / 1000LL),
        .tv_usec = (suseconds_t)((server_epoch_ms % 1000LL) * 1000LL) };
    if (settimeofday(&wall, NULL) != 0) {
        ESP_LOGW(TAG, "RTC wall clock could not be synchronized");
    }
    ESP_LOGI(TAG, "applied revision=%lu enabled=%d windows=%u",
             (unsigned long)revision, (int)enabled, (unsigned)window_count);
    return ESP_OK;
}

bool baby_tank_policy_is_locked(void)
{
    portENTER_CRITICAL(&s_lock);
    bool result = locked_now();
    portEXIT_CRITICAL(&s_lock);
    return result;
}

bool baby_tank_policy_is_daytime(void)
{
    portENTER_CRITICAL(&s_lock);
    bool daytime = false;
    if (s_shanghai_daily_rule && s_synced_us != 0) {
        daytime = baby_tank_shanghai_presentation_daytime(
            estimated_epoch_ms());
    } else if (s_synced_us != 0 && s_window_count > 0U) {
        int64_t now = estimated_epoch_ms();
        if (now < s_valid_until_epoch_ms) {
            for (size_t index = 0; index < s_window_count; ++index) {
                if (now < s_windows[index].start_epoch_ms) {
                    daytime = s_windows[index].start_epoch_ms - now >=
                        2LL * 60LL * 60LL * 1000LL;
                    break;
                }
                if (now < s_windows[index].end_epoch_ms) break;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return daytime;
}

bool baby_tank_policy_time_known(void)
{
    portENTER_CRITICAL(&s_lock);
    bool known = s_synced_us != 0 &&
        (s_shanghai_daily_rule ||
         estimated_epoch_ms() < s_valid_until_epoch_ms);
    portEXIT_CRITICAL(&s_lock);
    return known;
}

bool baby_tank_policy_tick(void)
{
    portENTER_CRITICAL(&s_lock);
    bool current = locked_now();
    bool entered = current && !s_last_locked;
    s_last_locked = current;
    portEXIT_CRITICAL(&s_lock);
    if (entered) ESP_LOGW(TAG, "deep-night gate entered");
    return entered;
}

uint32_t baby_tank_policy_revision(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t revision = s_revision;
    portEXIT_CRITICAL(&s_lock);
    return revision;
}

const char *baby_tank_policy_clock_quality(void)
{
    portENTER_CRITICAL(&s_lock);
    const char *quality = "unknown";
    if (s_synced_us != 0) {
        int64_t age_ms = (esp_timer_get_time() - s_synced_us) / 1000LL;
        if (s_shanghai_daily_rule ||
            estimated_epoch_ms() < s_valid_until_epoch_ms) {
            quality = !s_restored_clock && age_ms < POLICY_VERIFIED_MS
                ? "verified" : "estimated";
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return quality;
}

esp_err_t baby_tank_policy_usb_sync_shanghai_clock(uint32_t revision,
                                                   int64_t host_epoch_ms)
{
    /* Physical USB recovery uses the owner's independently verified Shanghai
     * policy. Its daily recurrence works while power remains; a cold boot
     * without RTC time remains unknown until another trusted sync. */
    if (host_epoch_ms < 1700000000000LL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    bool allowed = s_enabled && s_revision == revision;
    portEXIT_CRITICAL(&s_lock);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    stored_policy_t stored = { .base = {
        .version = POLICY_STORED_VERSION, .enabled = 1U,
        .revision = revision, .server_epoch_ms = host_epoch_ms,
        .valid_until_epoch_ms = INT64_MAX },
        .shanghai_daily_rule = 1U };
    nvs_handle_t handle;
    esp_err_t result = nvs_open("baby_policy", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "policy", &stored, sizeof(stored));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    portENTER_CRITICAL(&s_lock);
    if (!s_enabled || s_revision != revision) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_server_epoch_ms = host_epoch_ms;
    s_synced_us = esp_timer_get_time();
    s_valid_until_epoch_ms = INT64_MAX;
    s_window_count = 0;
    s_shanghai_daily_rule = true;
    s_usb_clock_source = true;
    s_restored_clock = false;
    s_last_persisted_epoch_ms = host_epoch_ms;
    portEXIT_CRITICAL(&s_lock);
    struct timeval wall = { .tv_sec = (time_t)(host_epoch_ms / 1000LL),
        .tv_usec = (suseconds_t)((host_epoch_ms % 1000LL) * 1000LL) };
    (void)settimeofday(&wall, NULL);
    ESP_LOGW(TAG, "physical USB Shanghai clock synchronized for policy revision=%lu; daily 23:00-06:00 gate active",
             (unsigned long)revision);
    return ESP_OK;
}
