#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BABY_TANK_EXPRESSION_CALM = 0,
    BABY_TANK_EXPRESSION_HAPPY,
    BABY_TANK_EXPRESSION_LAUGHING,
    BABY_TANK_EXPRESSION_EXCITED,
    BABY_TANK_EXPRESSION_LOVE,
    BABY_TANK_EXPRESSION_CURIOUS,
    BABY_TANK_EXPRESSION_SURPRISED,
    BABY_TANK_EXPRESSION_SHY,
    BABY_TANK_EXPRESSION_PROUD,
    BABY_TANK_EXPRESSION_BORED,
    BABY_TANK_EXPRESSION_ANNOYED,
    BABY_TANK_EXPRESSION_COMPLAINING,
    BABY_TANK_EXPRESSION_ANGRY,
    BABY_TANK_EXPRESSION_FURIOUS,
    BABY_TANK_EXPRESSION_SAD,
    BABY_TANK_EXPRESSION_CRYING,
    BABY_TANK_EXPRESSION_WORRIED,
    BABY_TANK_EXPRESSION_CONFUSED,
    BABY_TANK_EXPRESSION_DIZZY,
    BABY_TANK_EXPRESSION_SICK,
    BABY_TANK_EXPRESSION_THINKING,
    BABY_TANK_EXPRESSION_SLEEPY,
    BABY_TANK_EXPRESSION_SLEEPING,
    BABY_TANK_EXPRESSION_BUBBLE,
    BABY_TANK_EXPRESSION_CANNON,
    BABY_TANK_EXPRESSION_LISTENING,
    BABY_TANK_EXPRESSION_PROCESSING,
    BABY_TANK_EXPRESSION_EATING,
    BABY_TANK_EXPRESSION_DRINKING,
    BABY_TANK_EXPRESSION_ROCK,
    BABY_TANK_EXPRESSION_PAPER,
    BABY_TANK_EXPRESSION_SCISSORS,
    BABY_TANK_EXPRESSION_FIGHTING,
    BABY_TANK_EXPRESSION_DICE_1,
    BABY_TANK_EXPRESSION_DICE_2,
    BABY_TANK_EXPRESSION_DICE_3,
    BABY_TANK_EXPRESSION_DICE_4,
    BABY_TANK_EXPRESSION_DICE_5,
    BABY_TANK_EXPRESSION_DICE_6,
    BABY_TANK_EXPRESSION_STRAWBERRY,
    BABY_TANK_EXPRESSION_APPLE,
    BABY_TANK_EXPRESSION_BANANA,
    BABY_TANK_EXPRESSION_CAKE,
    BABY_TANK_EXPRESSION_COOKIE,
    BABY_TANK_EXPRESSION_BREAD,
    BABY_TANK_EXPRESSION_RICE_BALL,
    BABY_TANK_EXPRESSION_ICE_CREAM,
    BABY_TANK_EXPRESSION_MILK,
    BABY_TANK_EXPRESSION_JUICE,
    BABY_TANK_EXPRESSION_WATER,
    BABY_TANK_EXPRESSION_YOGURT,
    BABY_TANK_EXPRESSION_SOY_MILK,
    BABY_TANK_EXPRESSION_COCOA,
    BABY_TANK_EXPRESSION_COUNT,
} baby_tank_expression_t;

typedef enum {
    BABY_TANK_THOUGHT_NONE = 0,
    BABY_TANK_THOUGHT_STAR,
    BABY_TANK_THOUGHT_MOON,
    BABY_TANK_THOUGHT_CLOUD,
    BABY_TANK_THOUGHT_HEART,
    BABY_TANK_THOUGHT_FLOWER,
    BABY_TANK_THOUGHT_ROCKET,
    BABY_TANK_THOUGHT_CAR,
    BABY_TANK_THOUGHT_BALL,
    BABY_TANK_THOUGHT_FISH,
    BABY_TANK_THOUGHT_BUBBLE,
    BABY_TANK_THOUGHT_QUESTION,
    BABY_TANK_THOUGHT_MUSIC,
} baby_tank_thought_t;

#define BABY_TANK_DRAWING_WIDTH 240U
#define BABY_TANK_DRAWING_HEIGHT 240U
typedef struct {
    uint16_t duration_ms;
    uint16_t width;
    uint16_t height;
    size_t pixel_count;
    uint16_t *pixels; /* RGB565 little-endian, allocated in PSRAM. */
} baby_tank_drawing_t;

esp_err_t baby_tank_display_init(void);
esp_err_t baby_tank_display_render_expression(baby_tank_expression_t expression,
                                              uint32_t elapsed_ms,
                                              uint32_t duration_ms,
                                              uint8_t intensity,
                                              uint8_t speech_level);
esp_err_t baby_tank_display_render_drawing(const baby_tank_drawing_t *drawing);
esp_err_t baby_tank_display_set_sleep_thought(baby_tank_thought_t thought);
/* Updates the 2-4 character, cloud-generated sleep caption. */
esp_err_t baby_tank_display_set_sleep_text(const char *utf8_text);

/* Temporarily directs both eyes toward a normalized -100..100 target. */
esp_err_t baby_tank_display_look_at(int8_t horizontal,
                                   int8_t vertical,
                                   uint32_t duration_ms);
const char *baby_tank_expression_name(baby_tank_expression_t expression);
