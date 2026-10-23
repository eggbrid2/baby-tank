#include "state_event_queue.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#define EVENT_QUEUE_CAPACITY 16U
#define EVENT_QUEUE_VERSION 1U
#define EVENT_PERSIST_MIN_US (60LL * 1000000LL)

typedef struct {
    char id[37];
    uint8_t kind;
} event_entry_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    event_entry_t entries[EVENT_QUEUE_CAPACITY];
} stored_events_t;

static const char *TAG = "baby_tank_events";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static stored_events_t s_queue = { .version = EVENT_QUEUE_VERSION };
static bool s_dirty;
static int64_t s_last_persist_us;

esp_err_t baby_tank_state_events_init(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("baby_events", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    stored_events_t loaded = {0};
    size_t size = sizeof(loaded);
    result = nvs_get_blob(handle, "queue", &loaded, &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    if (size != sizeof(loaded) || loaded.version != EVENT_QUEUE_VERSION ||
        loaded.count > EVENT_QUEUE_CAPACITY) return ESP_ERR_INVALID_SIZE;
    for (unsigned index = 0; index < loaded.count; ++index) {
        if (loaded.entries[index].id[36] != '\0' ||
            loaded.entries[index].kind < BABY_TANK_STATE_EVENT_SIDE_TOUCH ||
            loaded.entries[index].kind > BABY_TANK_STATE_EVENT_SCENE_COMPLETED) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_queue = loaded;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "restored %u pending character events", (unsigned)loaded.count);
    return ESP_OK;
}

esp_err_t baby_tank_state_event_enqueue(baby_tank_state_event_kind_t kind)
{
    if (kind != BABY_TANK_STATE_EVENT_SIDE_TOUCH &&
        kind != BABY_TANK_STATE_EVENT_SCENE_COMPLETED) return ESP_ERR_INVALID_ARG;
    uint32_t random[4];
    esp_fill_random(random, sizeof(random));
    char id[37];
    snprintf(id, sizeof(id), "%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
        (unsigned long)random[0], (unsigned long)(random[1] & 0xffffU),
        (unsigned long)(random[1] >> 16U),
        (unsigned long)(random[2] & 0xffffU),
        (unsigned long)random[3], (unsigned long)(random[2] >> 16U));
    bool dropped = false;
    portENTER_CRITICAL(&s_lock);
    if (s_queue.count == EVENT_QUEUE_CAPACITY) {
        memmove(&s_queue.entries[0], &s_queue.entries[1],
                (EVENT_QUEUE_CAPACITY - 1U) * sizeof(event_entry_t));
        --s_queue.count;
        dropped = true;
    }
    event_entry_t *entry = &s_queue.entries[s_queue.count++];
    memcpy(entry->id, id, sizeof(entry->id));
    entry->kind = (uint8_t)kind;
    s_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    if (dropped) ESP_LOGW(TAG, "offline queue full; oldest event dropped");
    return ESP_OK;
}

bool baby_tank_state_event_peek(char id[37],
                               baby_tank_state_event_kind_t *kind)
{
    if (id == NULL || kind == NULL) return false;
    portENTER_CRITICAL(&s_lock);
    bool exists = s_queue.count > 0U;
    if (exists) {
        memcpy(id, s_queue.entries[0].id, 37U);
        *kind = (baby_tank_state_event_kind_t)s_queue.entries[0].kind;
    }
    portEXIT_CRITICAL(&s_lock);
    return exists;
}

void baby_tank_state_event_mark_uploaded(const char *id)
{
    if (id == NULL) return;
    portENTER_CRITICAL(&s_lock);
    if (s_queue.count > 0U && strcmp(s_queue.entries[0].id, id) == 0) {
        memmove(&s_queue.entries[0], &s_queue.entries[1],
                (s_queue.count - 1U) * sizeof(event_entry_t));
        --s_queue.count;
        s_dirty = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t baby_tank_state_events_persist_if_dirty(void)
{
    int64_t now = esp_timer_get_time();
    stored_events_t snapshot;
    portENTER_CRITICAL(&s_lock);
    bool due = s_dirty && (s_last_persist_us == 0 ||
                           now - s_last_persist_us >= EVENT_PERSIST_MIN_US);
    if (due) snapshot = s_queue;
    portEXIT_CRITICAL(&s_lock);
    if (!due) return ESP_OK;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("baby_events", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "queue", &snapshot, sizeof(snapshot));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    portENTER_CRITICAL(&s_lock);
    s_last_persist_us = now;
    if (memcmp(&snapshot, &s_queue, sizeof(snapshot)) == 0) s_dirty = false;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
