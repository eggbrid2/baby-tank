#include "interaction_memory.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define FOOD_FIRST 39U
#define FOOD_COUNT 8U
#define DRINK_FIRST 47U
#define DRINK_COUNT 6U
#define EVENT_CAPACITY 8U
#define COMBO_WINDOW_US (60LL * 1000000LL)
#define POKE_BURST_US (8LL * 1000000LL)

typedef enum {
    INTERACTION_FEED = 0,
    INTERACTION_DRINK,
    INTERACTION_POKE,
    INTERACTION_GAME,
} interaction_kind_t;

typedef struct {
    interaction_kind_t kind;
    uint8_t value;
    int64_t at_us;
} interaction_event_t;

static const char *TAG = "baby_tank_memory";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static interaction_event_t s_events[EVENT_CAPACITY];
static uint8_t s_event_count;
static uint8_t s_event_next;
static uint8_t s_favorite_food;
static uint8_t s_less_food;
static uint8_t s_favorite_drink;
static uint8_t s_less_drink;
static uint8_t s_last_feed = UINT8_MAX;
static uint8_t s_repeat_feed;
static uint8_t s_last_game = UINT8_MAX;
static uint8_t s_game_streak;
static int64_t s_last_poke_us;
static uint8_t s_poke_burst;

static void remember_locked(interaction_kind_t kind, uint8_t value,
                            int64_t now_us)
{
    s_events[s_event_next] = (interaction_event_t){
        .kind = kind,
        .value = value,
        .at_us = now_us,
    };
    s_event_next = (uint8_t)((s_event_next + 1U) % EVENT_CAPACITY);
    if (s_event_count < EVENT_CAPACITY) ++s_event_count;
}

static bool recently_ate_locked(int64_t now_us)
{
    for (uint8_t offset = 0U; offset < s_event_count; ++offset) {
        uint8_t index = (uint8_t)((s_event_next + EVENT_CAPACITY - 1U - offset) %
                                  EVENT_CAPACITY);
        const interaction_event_t *event = &s_events[index];
        if (now_us - event->at_us > COMBO_WINDOW_US) return false;
        if (event->kind == INTERACTION_FEED) return true;
        if (event->kind == INTERACTION_DRINK) return false;
    }
    return false;
}

void baby_tank_interaction_memory_init(void)
{
    uint8_t favorite_food = (uint8_t)(FOOD_FIRST + esp_random() % FOOD_COUNT);
    uint8_t less_food = favorite_food;
    while (less_food == favorite_food) {
        less_food = (uint8_t)(FOOD_FIRST + esp_random() % FOOD_COUNT);
    }
    uint8_t favorite_drink =
        (uint8_t)(DRINK_FIRST + esp_random() % DRINK_COUNT);
    uint8_t less_drink = favorite_drink;
    while (less_drink == favorite_drink) {
        less_drink = (uint8_t)(DRINK_FIRST + esp_random() % DRINK_COUNT);
    }

    taskENTER_CRITICAL(&s_lock);
    memset(s_events, 0, sizeof(s_events));
    s_event_count = 0U;
    s_event_next = 0U;
    s_favorite_food = favorite_food;
    s_less_food = less_food;
    s_favorite_drink = favorite_drink;
    s_less_drink = less_drink;
    s_last_feed = UINT8_MAX;
    s_repeat_feed = 0U;
    s_last_game = UINT8_MAX;
    s_game_streak = 0U;
    s_last_poke_us = 0;
    s_poke_burst = 0U;
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG,
             "session preferences food=%u less_food=%u drink=%u less_drink=%u",
             favorite_food, less_food, favorite_drink, less_drink);
}

baby_tank_feed_reaction_t baby_tank_interaction_memory_note_feed(uint8_t item)
{
    if (item < FOOD_FIRST || item >= DRINK_FIRST + DRINK_COUNT) {
        return BABY_TANK_FEED_REACTION_NORMAL;
    }
    int64_t now_us = esp_timer_get_time();
    bool drink = item >= DRINK_FIRST;
    baby_tank_feed_reaction_t reaction = BABY_TANK_FEED_REACTION_NORMAL;

    taskENTER_CRITICAL(&s_lock);
    bool after_meal = drink && recently_ate_locked(now_us);
    if (item == s_last_feed) {
        if (s_repeat_feed < UINT8_MAX) ++s_repeat_feed;
    } else {
        s_last_feed = item;
        s_repeat_feed = 1U;
    }

    if (s_repeat_feed >= 3U) {
        reaction = BABY_TANK_FEED_REACTION_REPEATED;
        s_repeat_feed = 0U;
    } else if (after_meal) {
        reaction = BABY_TANK_FEED_REACTION_AFTER_MEAL_DRINK;
    } else if (item == (drink ? s_favorite_drink : s_favorite_food)) {
        reaction = BABY_TANK_FEED_REACTION_FAVORITE;
    } else if (item == (drink ? s_less_drink : s_less_food)) {
        reaction = BABY_TANK_FEED_REACTION_LESS_PREFERRED;
    }
    remember_locked(drink ? INTERACTION_DRINK : INTERACTION_FEED, item, now_us);
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "feed item=%u reaction=%u", item, (unsigned)reaction);
    return reaction;
}

uint8_t baby_tank_interaction_memory_note_poke(void)
{
    int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    if (s_last_poke_us == 0 || now_us - s_last_poke_us > POKE_BURST_US) {
        s_poke_burst = 0U;
    }
    s_last_poke_us = now_us;
    if (s_poke_burst < UINT8_MAX) ++s_poke_burst;
    uint8_t burst = s_poke_burst;
    remember_locked(INTERACTION_POKE, burst, now_us);
    taskEXIT_CRITICAL(&s_lock);
    return burst;
}

uint8_t baby_tank_interaction_memory_note_game(uint8_t result)
{
    if (result > 2U) return 0U;
    int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    if (result == s_last_game) {
        if (s_game_streak < UINT8_MAX) ++s_game_streak;
    } else {
        s_last_game = result;
        s_game_streak = 1U;
    }
    uint8_t streak = s_game_streak;
    remember_locked(INTERACTION_GAME, result, now_us);
    taskEXIT_CRITICAL(&s_lock);
    return streak;
}
