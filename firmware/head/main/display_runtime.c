/*
 * Stable build entry point for the Baby Tank display.
 *
 * display.c is kept as the hardware bring-up/history file. It is included
 * here with its legacy renderer renamed, then the current renderer and API
 * are layered on top. This prevents an editor holding the historical file
 * open from replacing the production animation implementation.
 */

#include "display.h"

#define BT_DRAW_SELECT(_1, _2, _3, _4, _5, NAME, ...) NAME
#define BT_DRAW_TWO(expression, elapsed) \
    draw_expression_legacy_runtime(expression, elapsed)
#define BT_DRAW_FOUR(expression, elapsed, duration, intensity) \
    draw_expression_legacy_runtime(expression, elapsed)
#define BT_DRAW_FIVE(expression, elapsed, duration, intensity, speech) \
    draw_expression_legacy_runtime(expression, elapsed)
#define draw_expression(...) \
    BT_DRAW_SELECT(__VA_ARGS__, BT_DRAW_FIVE, BT_DRAW_FOUR, \
                   BT_DRAW_INVALID_THREE, BT_DRAW_TWO, \
                   BT_DRAW_INVALID_ONE)(__VA_ARGS__)
#define update_life_motion update_life_motion_legacy_runtime
#define baby_tank_display_render_expression baby_tank_display_render_expression_legacy_runtime

#include "display.c"

#undef baby_tank_display_render_expression
#undef update_life_motion
#undef draw_expression
#undef BT_DRAW_FIVE
#undef BT_DRAW_FOUR
#undef BT_DRAW_TWO
#undef BT_DRAW_SELECT

#define FORCED_GAZE_MAX_X 18
#define FORCED_GAZE_MAX_Y 14
#define FORCED_GAZE_IN_MS 120U
#define FORCED_GAZE_OUT_MS 240U

static portMUX_TYPE s_gaze_override_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_gaze_override_x;
static int s_gaze_override_y;
static uint32_t s_gaze_override_until_ms;
static uint32_t s_gaze_override_generation;
static uint32_t s_handled_gaze_generation;
static bool s_forced_gaze_active;
static bool s_forced_gaze_returning;
static portMUX_TYPE s_thought_lock = portMUX_INITIALIZER_UNLOCKED;
static baby_tank_thought_t s_sleep_thought = BABY_TANK_THOUGHT_CLOUD;
static uint8_t s_sleep_glyphs[4] = {6U, 7U, 7U, UINT8_MAX};
static size_t s_sleep_glyph_count = 3U;
static baby_tank_dream_entry_t s_dream_deck[BABY_TANK_DREAM_DECK_CAPACITY];
static size_t s_dream_deck_count;
static size_t thought_caption_count(baby_tank_thought_t thought);
static void set_sleep_glyphs_for_thought(baby_tank_thought_t thought);

esp_err_t baby_tank_display_set_sleep_thought(baby_tank_thought_t thought)
{
    ESP_RETURN_ON_FALSE(thought >= BABY_TANK_THOUGHT_NONE &&
                            thought <= BABY_TANK_THOUGHT_KITE,
                        ESP_ERR_INVALID_ARG, TAG, "invalid thought icon");
    taskENTER_CRITICAL(&s_thought_lock);
    s_sleep_thought = thought;
    set_sleep_glyphs_for_thought(thought);
    taskEXIT_CRITICAL(&s_thought_lock);
    return ESP_OK;
}

static void begin_gaze_move(int target_x, int target_y,
                            uint32_t now_ms, uint32_t duration_ms)
{
    s_motion.gaze_start_x = s_motion.gaze_x;
    s_motion.gaze_start_y = s_motion.gaze_y;
    s_motion.gaze_target_x = target_x;
    s_motion.gaze_target_y = target_y;
    s_motion.saccade_start_ms = now_ms;
    s_motion.saccade_duration_ms = duration_ms;
    s_motion.saccade_active = true;
}

static bool advance_gaze_move(uint32_t now_ms)
{
    if (!s_motion.saccade_active) return true;
    uint32_t elapsed = now_ms - s_motion.saccade_start_ms;
    if (elapsed >= s_motion.saccade_duration_ms) {
        s_motion.gaze_x = s_motion.gaze_target_x;
        s_motion.gaze_y = s_motion.gaze_target_y;
        s_motion.saccade_active = false;
        return true;
    }
    float amount = smoothstep((float)elapsed /
                              (float)s_motion.saccade_duration_ms);
    s_motion.gaze_x = s_motion.gaze_start_x +
        rounded_float((float)(s_motion.gaze_target_x -
                              s_motion.gaze_start_x) * amount);
    s_motion.gaze_y = s_motion.gaze_start_y +
        rounded_float((float)(s_motion.gaze_target_y -
                              s_motion.gaze_start_y) * amount);
    return false;
}

esp_err_t baby_tank_display_look_at(int8_t horizontal,
                                   int8_t vertical,
                                   uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL && s_framebuffer != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    ESP_RETURN_ON_FALSE(horizontal >= -100 && horizontal <= 100 &&
                            vertical >= -100 && vertical <= 100,
                        ESP_ERR_INVALID_ARG, TAG,
                        "gaze target must be normalized to -100..100");
    if (duration_ms < 300U) duration_ms = 300U;
    if (duration_ms > 5000U) duration_ms = 5000U;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    taskENTER_CRITICAL(&s_gaze_override_lock);
    s_gaze_override_x = horizontal * FORCED_GAZE_MAX_X / 100;
    s_gaze_override_y = vertical * FORCED_GAZE_MAX_Y / 100;
    s_gaze_override_until_ms = now_ms + duration_ms;
    ++s_gaze_override_generation;
    if (s_gaze_override_generation == 0U) ++s_gaze_override_generation;
    taskEXIT_CRITICAL(&s_gaze_override_lock);
    return ESP_OK;
}

static bool premium_expression_uses_gaze(baby_tank_expression_t expression)
{
    switch (expression) {
    case BABY_TANK_EXPRESSION_CALM:
    case BABY_TANK_EXPRESSION_CURIOUS:
    case BABY_TANK_EXPRESSION_BORED:
    case BABY_TANK_EXPRESSION_COMPLAINING:
    case BABY_TANK_EXPRESSION_CONFUSED:
    case BABY_TANK_EXPRESSION_THINKING:
        return true;
    default:
        return false;
    }
}

static void update_life_motion(baby_tank_expression_t expression, uint32_t now_ms)
{
    if (s_motion.random_state == 0) {
        s_motion.random_state = esp_random() | 1U;
        s_motion.last_update_ms = now_ms;
        s_motion.next_saccade_ms = now_ms + random_between(350U, 1300U);
        s_motion.next_blink_ms = now_ms + random_between(1700U, 4300U);
    }

    float delta_seconds = (float)(now_ms - s_motion.last_update_ms) / 1000.0f;
    if (delta_seconds > 0.05f) {
        delta_seconds = 0.05f;
    }
    s_motion.delta_seconds = delta_seconds;
    s_motion.last_update_ms = now_ms;

    if (expression != s_motion.previous_expression) {
        s_motion.gaze_target_x = 0;
        s_motion.gaze_target_y = 0;
        s_motion.face_x = 0.0f;
        s_motion.face_y = 0.0f;
        s_motion.face_velocity_x = 0.0f;
        s_motion.face_velocity_y = 0.0f;
        s_motion.previous_expression = expression;
    }

    int forced_x;
    int forced_y;
    uint32_t forced_until_ms;
    uint32_t forced_generation;
    taskENTER_CRITICAL(&s_gaze_override_lock);
    forced_x = s_gaze_override_x;
    forced_y = s_gaze_override_y;
    forced_until_ms = s_gaze_override_until_ms;
    forced_generation = s_gaze_override_generation;
    taskEXIT_CRITICAL(&s_gaze_override_lock);
    bool forced_requested = forced_generation != 0U &&
                            !time_reached(now_ms, forced_until_ms);

    if (forced_requested) {
        if (forced_generation != s_handled_gaze_generation) {
            s_handled_gaze_generation = forced_generation;
            s_forced_gaze_active = true;
            s_forced_gaze_returning = false;
            begin_gaze_move(forced_x, forced_y, now_ms, FORCED_GAZE_IN_MS);
        }
        advance_gaze_move(now_ms);
        s_motion.next_saccade_ms = forced_until_ms + 500U;
    } else if (s_forced_gaze_active) {
        s_forced_gaze_active = false;
        s_forced_gaze_returning = true;
        begin_gaze_move(0, 0, now_ms, FORCED_GAZE_OUT_MS);
        advance_gaze_move(now_ms);
    } else if (s_forced_gaze_returning) {
        if (advance_gaze_move(now_ms)) {
            s_forced_gaze_returning = false;
            s_motion.next_saccade_ms = now_ms + random_between(650U, 1500U);
        }
    } else if (!premium_expression_uses_gaze(expression)) {
        s_motion.gaze_target_x = 0;
        s_motion.gaze_target_y = 0;
        s_motion.gaze_x = s_motion.gaze_x * 3 / 4;
        s_motion.gaze_y = s_motion.gaze_y * 3 / 4;
        s_motion.saccade_active = false;
        s_motion.next_saccade_ms = now_ms + random_between(900U, 1800U);
    } else if (s_motion.saccade_active) {
        if (advance_gaze_move(now_ms)) {
            s_motion.next_saccade_ms = now_ms + random_between(350U, 2400U);
        }
    } else if (time_reached(now_ms, s_motion.next_saccade_ms)) {
        s_motion.gaze_start_x = s_motion.gaze_x;
        s_motion.gaze_start_y = s_motion.gaze_y;
        s_motion.gaze_target_x = (int)random_between(0U, 8U) - 4;
        s_motion.gaze_target_y = (int)random_between(0U, 4U) - 2;
        if ((next_random() & 3U) == 0U) {
            s_motion.gaze_target_x = 0;
            s_motion.gaze_target_y = 0;
        }
        s_motion.saccade_duration_ms = random_between(75U, 145U);
        s_motion.saccade_start_ms = now_ms;
        s_motion.saccade_active = true;
    }

    if (s_motion.blink_active &&
        time_reached(now_ms, s_motion.blink_start_ms + 285U)) {
        s_motion.blink_active = false;
        if (s_motion.double_blink_pending) {
            s_motion.double_blink_pending = false;
            s_motion.next_blink_ms = now_ms + random_between(110U, 170U);
        } else {
            s_motion.next_blink_ms = now_ms + random_between(1800U, 5200U);
        }
    } else if (!s_motion.blink_active &&
               !expression_is_sleeping(expression) &&
               premium_expression_uses_gaze(expression) &&
               time_reached(now_ms, s_motion.next_blink_ms)) {
        s_motion.blink_start_ms = now_ms;
        s_motion.blink_active = true;
        s_motion.double_blink_pending = (next_random() % 7U) == 0U;
    }

    s_canvas_offset_x = 0;
    s_canvas_offset_y = 0;
    s_eye_offset_x = s_motion.gaze_x;
    s_eye_offset_y = s_motion.gaze_y;
    s_left_blink = blink_amount(now_ms, 0U);
    s_right_blink = blink_amount(now_ms, 14U);
}

#include "premium_face.inc"
#include "thought_font.inc"

static uint16_t thought_alpha_color(uint16_t color, uint8_t alpha)
{
    uint8_t red = (uint8_t)((color >> 11) & 0x1fU);
    uint8_t green = (uint8_t)((color >> 5) & 0x3fU);
    uint8_t blue = (uint8_t)(color & 0x1fU);
    red = (uint8_t)(red * alpha / 15U);
    green = (uint8_t)(green * alpha / 15U);
    blue = (uint8_t)(blue * alpha / 15U);
    return (uint16_t)((uint16_t)red << 11 | (uint16_t)green << 5 | blue);
}

static uint16_t thought_blend_color(uint16_t from, uint16_t to,
                                    uint8_t amount)
{
    uint16_t inverse = 255U - amount;
    uint8_t from_red = (uint8_t)((from >> 11) & 0x1fU);
    uint8_t from_green = (uint8_t)((from >> 5) & 0x3fU);
    uint8_t from_blue = (uint8_t)(from & 0x1fU);
    uint8_t to_red = (uint8_t)((to >> 11) & 0x1fU);
    uint8_t to_green = (uint8_t)((to >> 5) & 0x3fU);
    uint8_t to_blue = (uint8_t)(to & 0x1fU);
    uint8_t red = (uint8_t)((from_red * inverse + to_red * amount) / 255U);
    uint8_t green = (uint8_t)((from_green * inverse + to_green * amount) / 255U);
    uint8_t blue = (uint8_t)((from_blue * inverse + to_blue * amount) / 255U);
    return (uint16_t)((uint16_t)red << 11 | (uint16_t)green << 5 | blue);
}

static uint8_t thought_glyph_alpha(uint8_t glyph, int row, int column)
{
    if (row < 0 || row >= 18 || column < 0 || column >= 18) return 0U;
    uint8_t packed = s_thought_glyph_alpha[glyph][row][column / 2];
    return (column & 1) == 0 ? packed >> 4 : packed & 0x0fU;
}

static const uint8_t s_thought_captions[][4] = {
    [BABY_TANK_THOUGHT_STAR] = {0, 1, 1, UINT8_MAX},
    [BABY_TANK_THOUGHT_MOON] = {2, 3, 4, 5},
    [BABY_TANK_THOUGHT_CLOUD] = {6, 7, 7, UINT8_MAX},
    [BABY_TANK_THOUGHT_HEART] = {8, 9, 9, UINT8_MAX},
    [BABY_TANK_THOUGHT_FLOWER] = {10, 11, 12, UINT8_MAX},
    [BABY_TANK_THOUGHT_ROCKET] = {13, 14, 15, UINT8_MAX},
    [BABY_TANK_THOUGHT_CAR] = {16, 13, 17, UINT8_MAX},
    [BABY_TANK_THOUGHT_BALL] = {18, 19, 18, UINT8_MAX},
    [BABY_TANK_THOUGHT_FISH] = {20, 21, 22, UINT8_MAX},
    [BABY_TANK_THOUGHT_BUBBLE] = {23, 24, 24, UINT8_MAX},
    [BABY_TANK_THOUGHT_QUESTION] = {8, 19, 8, UINT8_MAX},
    [BABY_TANK_THOUGHT_MUSIC] = {25, 20, 26, UINT8_MAX},
    [BABY_TANK_THOUGHT_BIRD] = {16, 13, 17, UINT8_MAX},
    [BABY_TANK_THOUGHT_BOOK] = {8, 19, 8, UINT8_MAX},
    [BABY_TANK_THOUGHT_KITE] = {13, 14, 15, UINT8_MAX},
};

static const uint16_t s_thought_codepoints[] = {
    0x4EAE,0x6676,0x505A,0x4E2A,0x597D,0x68A6,0x8F6F,0x7EF5,0x60F3,
    0x62B1,0x82B1,0x5F00,0x5566,0x53BB,0x63A2,0x9669,0x51FA,0x73A9,
    0x6EDA,0x4E00,0x5C0F,0x9C7C,0x6E38,0x5439,0x6CE1,0x54FC,0x66F2,
};

static void set_sleep_glyphs_for_thought(baby_tank_thought_t thought)
{
    size_t count = thought_caption_count(thought);
    s_sleep_glyph_count = count;
    for (size_t index = 0U; index < 4U; ++index) {
        s_sleep_glyphs[index] = index < count
            ? s_thought_captions[thought][index] : UINT8_MAX;
    }
}

static bool next_utf8_codepoint(const char **cursor, uint32_t *codepoint)
{
    const uint8_t *bytes = (const uint8_t *)*cursor;
    if (bytes[0] == 0U) return false;
    if (bytes[0] < 0x80U) {
        *codepoint = bytes[0];
        *cursor += 1;
        return true;
    }
    if ((bytes[0] & 0xE0U) == 0xC0U && bytes[1] != 0U &&
        (bytes[1] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(bytes[0] & 0x1FU) << 6U) |
                     (uint32_t)(bytes[1] & 0x3FU);
        *cursor += 2;
        return *codepoint >= 0x80U;
    }
    if ((bytes[0] & 0xF0U) == 0xE0U && bytes[1] != 0U &&
        (bytes[1] & 0xC0U) == 0x80U &&
        bytes[2] != 0U &&
        (bytes[2] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(bytes[0] & 0x0FU) << 12U) |
                     ((uint32_t)(bytes[1] & 0x3FU) << 6U) |
                     (uint32_t)(bytes[2] & 0x3FU);
        *cursor += 3;
        return *codepoint >= 0x800U;
    }
    return false;
}

static baby_tank_thought_t thought_from_key(const char *key)
{
    static const struct {
        const char *key;
        baby_tank_thought_t thought;
    } values[] = {
        {"star", BABY_TANK_THOUGHT_STAR}, {"moon", BABY_TANK_THOUGHT_MOON},
        {"cloud", BABY_TANK_THOUGHT_CLOUD}, {"heart", BABY_TANK_THOUGHT_HEART},
        {"flower", BABY_TANK_THOUGHT_FLOWER}, {"rocket", BABY_TANK_THOUGHT_ROCKET},
        {"car", BABY_TANK_THOUGHT_CAR}, {"ball", BABY_TANK_THOUGHT_BALL},
        {"fish", BABY_TANK_THOUGHT_FISH}, {"bubble", BABY_TANK_THOUGHT_BUBBLE},
        {"question", BABY_TANK_THOUGHT_QUESTION}, {"music", BABY_TANK_THOUGHT_MUSIC},
        {"bird", BABY_TANK_THOUGHT_BIRD}, {"book", BABY_TANK_THOUGHT_BOOK},
        {"kite", BABY_TANK_THOUGHT_KITE},
    };
    if (key == NULL) return BABY_TANK_THOUGHT_NONE;
    for (size_t index = 0U; index < sizeof(values) / sizeof(values[0]); ++index) {
        if (strcmp(values[index].key, key) == 0) return values[index].thought;
    }
    return BABY_TANK_THOUGHT_NONE;
}

static esp_err_t glyphs_from_text(const char *utf8_text, uint8_t glyphs[4],
                                  uint8_t *glyph_count)
{
    ESP_RETURN_ON_FALSE(utf8_text != NULL && glyphs != NULL &&
                            glyph_count != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "dream text is required");
    memset(glyphs, UINT8_MAX, 4U);
    size_t count = 0U;
    const char *cursor = utf8_text;
    while (*cursor != '\0') {
        ESP_RETURN_ON_FALSE(count < 4U, ESP_ERR_INVALID_SIZE, TAG,
                            "dream text exceeds four characters");
        uint32_t codepoint = 0U;
        const char *before = cursor;
        ESP_RETURN_ON_FALSE(next_utf8_codepoint(&cursor, &codepoint) &&
                                cursor > before,
                            ESP_ERR_INVALID_ARG, TAG,
                            "dream text contains invalid UTF-8");
        size_t glyph = 0U;
        while (glyph < sizeof(s_thought_codepoints) /
                           sizeof(s_thought_codepoints[0]) &&
               s_thought_codepoints[glyph] != codepoint) {
            ++glyph;
        }
        ESP_RETURN_ON_FALSE(glyph < sizeof(s_thought_codepoints) /
                                       sizeof(s_thought_codepoints[0]),
                            ESP_ERR_NOT_SUPPORTED, TAG,
                            "dream text glyph is not embedded");
        glyphs[count++] = (uint8_t)glyph;
    }
    ESP_RETURN_ON_FALSE(count >= 2U, ESP_ERR_INVALID_SIZE, TAG,
                        "dream text needs two to four characters");
    *glyph_count = (uint8_t)count;
    return ESP_OK;
}

esp_err_t baby_tank_display_set_sleep_text(const char *utf8_text)
{
    uint8_t glyphs[4] = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX};
    uint8_t count = 0U;
    ESP_RETURN_ON_ERROR(glyphs_from_text(utf8_text, glyphs, &count), TAG,
                        "invalid sleep text");
    taskENTER_CRITICAL(&s_thought_lock);
    memcpy(s_sleep_glyphs, glyphs, sizeof(glyphs));
    s_sleep_glyph_count = count;
    taskEXIT_CRITICAL(&s_thought_lock);
    ESP_LOGI(TAG, "cloud sleep caption updated glyphs=%u", (unsigned)count);
    return ESP_OK;
}

esp_err_t baby_tank_display_make_dream_entry(const char *key,
                                             const char *utf8_text,
                                             baby_tank_dream_entry_t *entry)
{
    ESP_RETURN_ON_FALSE(entry != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "dream entry output is required");
    baby_tank_thought_t thought = thought_from_key(key);
    ESP_RETURN_ON_FALSE(thought != BABY_TANK_THOUGHT_NONE,
                        ESP_ERR_INVALID_ARG, TAG, "unknown dream icon");
    baby_tank_dream_entry_t parsed = {.thought = thought};
    ESP_RETURN_ON_ERROR(glyphs_from_text(utf8_text, parsed.glyphs,
                                        &parsed.glyph_count),
                        TAG, "invalid dream caption");
    *entry = parsed;
    return ESP_OK;
}

esp_err_t baby_tank_display_set_dream_deck(
    const baby_tank_dream_entry_t *entries, size_t count)
{
    ESP_RETURN_ON_FALSE(entries != NULL && count > 0U &&
                            count <= BABY_TANK_DREAM_DECK_CAPACITY,
                        ESP_ERR_INVALID_ARG, TAG, "invalid dream deck");
    for (size_t index = 0U; index < count; ++index) {
        ESP_RETURN_ON_FALSE(entries[index].thought > BABY_TANK_THOUGHT_NONE &&
                                entries[index].thought <= BABY_TANK_THOUGHT_KITE &&
                                entries[index].glyph_count >= 2U &&
                                entries[index].glyph_count <= 4U,
                            ESP_ERR_INVALID_ARG, TAG, "invalid dream card");
    }
    taskENTER_CRITICAL(&s_thought_lock);
    memcpy(s_dream_deck, entries, count * sizeof(entries[0]));
    s_dream_deck_count = count;
    taskEXIT_CRITICAL(&s_thought_lock);
    ESP_LOGI(TAG, "dream deck activated cards=%u", (unsigned)count);
    return ESP_OK;
}

static size_t thought_caption_count(baby_tank_thought_t thought)
{
    if (thought <= BABY_TANK_THOUGHT_NONE ||
        thought > BABY_TANK_THOUGHT_KITE) return 0U;
    size_t count = 0U;
    while (count < 4U && s_thought_captions[thought][count] != UINT8_MAX) {
        ++count;
    }
    return count;
}

static void draw_thought_glyph(uint8_t glyph, int left, int top_q8,
                               uint16_t color)
{
    if (glyph >= sizeof(s_thought_glyph_alpha) /
                     sizeof(s_thought_glyph_alpha[0])) return;
    int top = top_q8 >> 8;
    int fraction = top_q8 & 0xff;
    uint16_t palette[16];
    for (unsigned index = 0; index < 16U; ++index) {
        palette[index] = thought_alpha_color(color, (uint8_t)index);
    }
    for (int output_row = 0; output_row < 19; ++output_row) {
        int source_q8 = output_row * 256 - fraction;
        int source_row = source_q8 >= 0 ? source_q8 >> 8 : -1;
        int blend = source_q8 - source_row * 256;
        for (int column = 0; column < 18; ++column) {
            int first = thought_glyph_alpha(glyph, source_row, column);
            int second = thought_glyph_alpha(glyph, source_row + 1, column);
            int alpha = (first * (256 - blend) + second * blend + 128) >> 8;
            if (alpha > 0) set_pixel(left + column, top + output_row,
                                     palette[alpha]);
        }
    }
}

static void draw_thought_text(baby_tank_thought_t thought, int center_x,
                              int top, uint16_t color, uint32_t text_ms)
{
    size_t full_count = thought_caption_count(thought);
    if (full_count == 0U) return;
    int left = center_x - ((int)full_count * 18 +
                           ((int)full_count - 1)) / 2;
    for (size_t index = 0; index < full_count; ++index) {
        int32_t glyph_ms = (int32_t)text_ms - (int32_t)index * 170;
        if (glyph_ms < 0) continue;
        float entrance = smoothstep((float)glyph_ms / 420.0f);
        int settle_q8 = rounded_float((1.0f - entrance) * 7.0f * 256.0f);
        int breathe_q8 = 0;
        if (glyph_ms > 700) {
            uint32_t phase = ((uint32_t)glyph_ms +
                              (uint32_t)index * 130U) % 2800U;
            float unit = (float)phase / 2800.0f;
            float triangle = unit < 0.5f ? unit * 2.0f :
                                           (1.0f - unit) * 2.0f;
            breathe_q8 = rounded_float((smoothstep(triangle) * 2.0f - 1.0f) *
                                       0.7f * 256.0f);
        }
        draw_thought_glyph(s_thought_captions[thought][index],
                           left + (int)index * 19,
                           top * 256 + settle_q8 + breathe_q8, color);
    }
}

static void draw_thought_text_scaled(const uint8_t *glyphs, size_t count,
                                     int center_x, int center_y,
                                     int scale_1000, uint16_t color,
                                     uint32_t text_ms)
{
    if (glyphs == NULL || count == 0U || count > 4U || scale_1000 < 120) return;
    if (scale_1000 > 1000) scale_1000 = 1000;
    int source_width = (int)count * 18 + ((int)count - 1);
    int output_width = source_width * scale_1000 / 1000;
    int output_height = 18 * scale_1000 / 1000;
    if (output_width < 1 || output_height < 1) return;
    int left = center_x - output_width / 2;
    int top = center_y - output_height / 2;
    uint16_t palette[16];
    for (unsigned alpha = 0U; alpha < 16U; ++alpha) {
        palette[alpha] = thought_alpha_color(color, (uint8_t)alpha);
    }
    for (int out_y = 0; out_y < output_height; ++out_y) {
        int source_y = out_y * 18 / output_height;
        for (int out_x = 0; out_x < output_width; ++out_x) {
            int source_x = out_x * source_width / output_width;
            int glyph_slot = source_x / 19;
            int glyph_x = source_x - glyph_slot * 19;
            if (glyph_slot >= (int)count || glyph_x >= 18 ||
                text_ms < (uint32_t)glyph_slot * 90U) continue;
            uint8_t alpha = thought_glyph_alpha(
                glyphs[glyph_slot], source_y, glyph_x);
            if (alpha > 0U) set_pixel(left + out_x, top + out_y,
                                      palette[alpha]);
        }
    }
}

static void draw_sleep_z(int left, int top, int size, uint16_t color)
{
    int thickness = size >= 10 ? 2 : 1;
    draw_line(left, top, left + size, top, thickness, color);
    draw_line(left + size, top, left, top + size, thickness, color);
    draw_line(left, top + size, left + size, top + size, thickness, color);
}

static void draw_sleep_z_drift(uint32_t phase_ms, uint32_t start_ms,
                               int left, int top, int size,
                               uint16_t color)
{
    if (phase_ms < start_ms || phase_ms >= start_ms + 1750U) return;
    uint32_t local_ms = phase_ms - start_ms;
    float travel = smoothstep((float)local_ms / 1750.0f);
    float presence;
    if (local_ms < 330U) {
        presence = smoothstep((float)local_ms / 330.0f);
    } else if (local_ms > 1330U) {
        presence = 1.0f - smoothstep((float)(local_ms - 1330U) / 420.0f);
    } else {
        presence = 1.0f;
    }
    int rendered_size = rounded_float((float)size *
                                      (0.35f + presence * 0.65f));
    int rise = rounded_float(travel * 20.0f);
    int sway = smooth_wave(local_ms + start_ms, 1500U, 2);
    draw_sleep_z(left + sway, top - rise, rendered_size, color);
}

static void __attribute__((unused)) draw_sleep_zzz(
    uint32_t phase_ms, uint16_t cyan, uint16_t cream)
{
    draw_sleep_z_drift(phase_ms, 0U, 118, 79, 7, cream);
    draw_sleep_z_drift(phase_ms, 390U, 141, 62, 10, cyan);
    if (phase_ms < 1500U) {
        draw_sleep_z_drift(phase_ms, 820U, 168, 43, 13, cyan);
    }
}

static void __attribute__((unused)) draw_sleep_z_to_cloud(
    uint32_t phase_ms, uint16_t cyan, uint16_t cream)
{
    static const int16_t starts[12][2] = {
        /* Exact screen position of the final drifting Z at 1500 ms. */
        {166, 36}, {171, 36}, {176, 36}, {179, 36},
        {177, 39}, {174, 42}, {171, 45}, {168, 48},
        {166, 49}, {171, 49}, {176, 49}, {179, 49},
    };
    static const int16_t targets[12][2] = {
        {132, 49}, {132, 49}, {145, 39}, {145, 39},
        {168, 34}, {168, 34}, {168, 34},
        {191, 38}, {191, 38}, {207, 49}, {207, 49},
        {169, 52},
    };
    if (phase_ms < 1500U || phase_ms >= 3400U) return;
    uint32_t local_ms = phase_ms - 1500U;

    for (unsigned index = 0; index < 12U; ++index) {
        uint32_t delay_ms = (index % 4U) * 45U;
        uint32_t moving_ms = local_ms > delay_ms ? local_ms - delay_ms : 0U;
        float progress = smoothstep((float)moving_ms / 1765.0f);
        if (progress > 1.0f) progress = 1.0f;
        float arc = 4.0f * progress * (1.0f - progress);
        int x = starts[index][0] + rounded_float(
            (float)(targets[index][0] - starts[index][0]) * progress);
        int y = starts[index][1] + rounded_float(
            (float)(targets[index][1] - starts[index][1]) * progress -
            arc * (3.0f + (float)(index % 3U)));
        int radius = 1 + rounded_float(progress *
                                      (index % 3U == 0U ? 3.0f : 2.0f));
        uint8_t mix = (uint8_t)rounded_float(progress * 255.0f);
        uint16_t color = thought_blend_color(cyan, cream, mix);
        fill_circle(x, y, radius, color);
        if (radius >= 3 && progress > 0.72f) {
            fill_circle(x, y, radius - 2, rgb565(0, 0, 0));
        }
    }
}

static int scale_around(int center, int value, int scale_1000)
{
    return center + (value - center) * scale_1000 / 1000;
}

static void __attribute__((unused)) draw_sleep_cloud(
    int cloud_y, baby_tank_thought_t thought,
    int scale_x, int scale_y, bool show_text,
    int formation_1000, uint32_t cloud_ms,
    uint32_t text_ms,
    uint16_t black, uint16_t cyan, uint16_t cream)
{
    static const int16_t outer_x[5] = {132, 145, 168, 191, 207};
    static const int8_t outer_y[5] = {5, -5, -10, -6, 5};
    static const int8_t outer_radius[5] = {15, 18, 22, 19, 15};
    static const int16_t inner_x[5] = {134, 147, 168, 190, 205};
    static const int8_t inner_y[5] = {5, -4, -7, -4, 5};
    static const int8_t inner_radius[5] = {12, 15, 18, 16, 12};
    const int center_x = 169;
    const int center_y = cloud_y + 4;
    int drift_x[5];
    int drift_y[5];
    int radius_delta[5];
    int outer_draw_radius[5];
    int inner_draw_radius[5];
    static const int16_t formation_delay[5] = {40, 0, 80, 30, 100};
    for (unsigned index = 0; index < 5U; ++index) {
        drift_x[index] = smooth_wave(cloud_ms + index * 347U,
                                     3100U + index * 170U, 1);
        drift_y[index] = smooth_wave(cloud_ms + index * 521U,
                                     2500U + index * 230U, 1);
        radius_delta[index] = smooth_wave(cloud_ms + index * 293U,
                                          3600U + index * 190U, 1);
    }
#define CLOUD_X(value) scale_around(center_x, (value), scale_x)
#define CLOUD_Y(value) scale_around(center_y, (value), scale_y)
#define CLOUD_R(value) (((value) * (scale_x < scale_y ? scale_x : scale_y) / 1000) > 1 ? \
                        ((value) * (scale_x < scale_y ? scale_x : scale_y) / 1000) : 1)
    uint16_t feather = thought_alpha_color(cream, 5U);
    for (unsigned index = 0; index < 5U; ++index) {
        int available = formation_1000 - formation_delay[index];
        float growth = available > 0 ? smoothstep(
            (float)available / (float)(1000 - formation_delay[index])) : 0.0f;
        int full_outer = CLOUD_R(outer_radius[index] + radius_delta[index]);
        outer_draw_radius[index] = growth > 0.0f ?
            2 + rounded_float((float)(full_outer - 2) * growth) : 0;
        float hollow = smoothstep((float)(formation_1000 - 560) / 440.0f);
        int full_inner = CLOUD_R(inner_radius[index] + radius_delta[index]);
        inner_draw_radius[index] = hollow > 0.0f ?
            rounded_float((float)full_inner * hollow) : 0;
    }
    /* Each puff has its own slow phase; the cloud never moves as a rigid plate. */
    for (unsigned index = 0; index < 5U; ++index) {
        if (outer_draw_radius[index] <= 0) continue;
        fill_circle(CLOUD_X(outer_x[index] + drift_x[index]),
                    CLOUD_Y(cloud_y + outer_y[index] + drift_y[index]),
                    outer_draw_radius[index] + 1,
                    feather);
    }
    float bridge = smoothstep((float)(formation_1000 - 300) / 700.0f);
    if (bridge > 0.0f) {
        int bridge_width = 8 + rounded_float(84.0f * bridge);
        int bridge_height = 4 + rounded_float(24.0f * bridge);
        fill_round_rect_center(center_x, CLOUD_Y(cloud_y + 8),
                               bridge_width * scale_x / 1000 + 2,
                               bridge_height * scale_y / 1000 + 2,
                               CLOUD_R(2 + rounded_float(11.0f * bridge)) + 1,
                               feather);
    }
    for (unsigned index = 0; index < 5U; ++index) {
        if (outer_draw_radius[index] <= 0) continue;
        fill_circle(CLOUD_X(outer_x[index] + drift_x[index]),
                    CLOUD_Y(cloud_y + outer_y[index] + drift_y[index]),
                    outer_draw_radius[index], cream);
    }
    if (bridge > 0.0f) {
        int bridge_width = 8 + rounded_float(84.0f * bridge);
        int bridge_height = 4 + rounded_float(24.0f * bridge);
        fill_round_rect_center(center_x, CLOUD_Y(cloud_y + 8),
                               bridge_width * scale_x / 1000,
                               bridge_height * scale_y / 1000,
                               CLOUD_R(2 + rounded_float(11.0f * bridge)),
                               cream);
    }
    if (formation_1000 > 560 && scale_x >= 520 && scale_y >= 520) {
        for (unsigned index = 0; index < 5U; ++index) {
            if (inner_draw_radius[index] <= 0) continue;
            fill_circle(CLOUD_X(inner_x[index] + drift_x[index]),
                        CLOUD_Y(cloud_y + inner_y[index] + drift_y[index]),
                        inner_draw_radius[index] + 1,
                        feather);
        }
        float hollow = smoothstep((float)(formation_1000 - 560) / 440.0f);
        int hollow_width = rounded_float(86.0f * hollow);
        int hollow_height = rounded_float(23.0f * hollow);
        if (hollow_width > 2 && hollow_height > 2) {
            fill_round_rect_center(center_x, CLOUD_Y(cloud_y + 8),
                                   hollow_width * scale_x / 1000,
                                   hollow_height * scale_y / 1000,
                                   CLOUD_R(rounded_float(10.0f * hollow)),
                                   feather);
        }
        for (unsigned index = 0; index < 5U; ++index) {
            int radius = inner_draw_radius[index] - 1;
            if (radius < 1) radius = 1;
            fill_circle(CLOUD_X(inner_x[index] + drift_x[index]),
                        CLOUD_Y(cloud_y + inner_y[index] + drift_y[index]),
                        radius, black);
        }
        if (hollow_width > 4 && hollow_height > 4) {
            fill_round_rect_center(center_x, CLOUD_Y(cloud_y + 8),
                                   (hollow_width - 2) * scale_x / 1000,
                                   (hollow_height - 2) * scale_y / 1000,
                                   CLOUD_R(rounded_float(9.0f * hollow)), black);
        }
    }
    if (formation_1000 > 420 && scale_x > 820 && scale_y > 820) {
        int tail_sway = smooth_wave(cloud_ms, 3300U, 1);
        float tail_growth = smoothstep((float)(formation_1000 - 420) / 580.0f);
        int large_tail = 1 + rounded_float(5.0f * tail_growth);
        int small_tail = 1 + rounded_float(2.0f * tail_growth);
        uint16_t tail_feather = thought_alpha_color(cream, 5U);
        fill_circle(CLOUD_X(124 + tail_sway), CLOUD_Y(cloud_y + 30),
                    CLOUD_R(large_tail + 1), tail_feather);
        fill_circle(CLOUD_X(124 + tail_sway), CLOUD_Y(cloud_y + 30),
                    CLOUD_R(large_tail), cream);
        if (formation_1000 > 700) {
            int tail_hole = rounded_float(3.0f * smoothstep(
                (float)(formation_1000 - 700) / 300.0f));
            if (tail_hole > 0) {
                fill_circle(CLOUD_X(124 + tail_sway), CLOUD_Y(cloud_y + 30),
                            CLOUD_R(tail_hole), black);
            }
        }
        fill_circle(CLOUD_X(114 - tail_sway), CLOUD_Y(cloud_y + 41),
                    CLOUD_R(small_tail + 1), tail_feather);
        fill_circle(CLOUD_X(114 - tail_sway), CLOUD_Y(cloud_y + 41),
                    CLOUD_R(small_tail), cream);
    }
    if (show_text && formation_1000 >= 1000 &&
        scale_x > 930 && scale_y > 930) {
        draw_thought_text(thought, center_x, cloud_y - 9,
                          cyan, text_ms);
    }
#undef CLOUD_R
#undef CLOUD_Y
#undef CLOUD_X
}

static void __attribute__((unused)) draw_sleep_cloud_pop(
    uint32_t pop_ms, uint16_t cyan, uint16_t cream)
{
    static const int8_t directions[10][2] = {
        {-100, -10}, {-82, -58}, {-38, -92}, {22, -96}, {78, -65},
        {100, -5}, {82, 54}, {38, 86}, {-35, 88}, {-84, 52},
    };
    float progress = smoothstep((float)pop_ms / 720.0f);
    const int center_x = 169;
    const int center_y = 48;

    for (unsigned index = 0; index < 10U; ++index) {
        uint32_t delay_ms = (index % 3U) * 45U;
        if (pop_ms <= delay_ms) continue;
        float particle = smoothstep((float)(pop_ms - delay_ms) / 675.0f);
        int distance = 8 + rounded_float(particle * (28.0f +
                                      (float)(index % 4U) * 3.0f));
        int x = center_x + directions[index][0] * distance / 100;
        int y = center_y + directions[index][1] * distance / 100;
        int radius = rounded_float((1.0f - progress) * 3.0f) + 1;
        draw_bubble(x, y, radius, index % 2U == 0U ? cream : cyan);
    }
}

static void draw_travelling_sleep_z(uint32_t phase_ms, uint32_t start_ms,
                                    uint32_t duration_ms,
                                    int start_x, int start_y,
                                    int end_x, int end_y,
                                    int size, bool keep_last_frame,
                                    uint16_t color)
{
    if (phase_ms < start_ms || phase_ms >= start_ms + duration_ms) return;
    uint32_t local_ms = phase_ms - start_ms;
    float travel = smoothstep((float)local_ms / (float)duration_ms);
    float arc = 4.0f * travel * (1.0f - travel);
    int x = start_x + rounded_float((float)(end_x - start_x) * travel);
    int y = start_y + rounded_float((float)(end_y - start_y) * travel -
                                    arc * 3.0f);
    float presence = smoothstep((float)local_ms / 180.0f);
    if (!keep_last_frame && local_ms + 300U > duration_ms) {
        presence *= 1.0f - smoothstep(
            (float)(local_ms + 300U - duration_ms) / 300.0f);
    }
    uint8_t alpha = (uint8_t)rounded_float(presence * 15.0f);
    if (alpha == 0U) return;
    draw_sleep_z(x, y, size, thought_alpha_color(color, alpha));
}

static void draw_dream_icon(baby_tank_thought_t thought, int center_x,
                            int center_y, int scale, uint32_t animation_ms,
                            uint16_t ink, uint16_t accent, uint16_t dark)
{
    int bob = smooth_wave(animation_ms, 2200U, scale >= 10 ? 2 : 1);
    int flap = (animation_ms / 260U) % 2U == 0U ? 2 : -1;
    int s = scale < 6 ? 6 : scale;
    center_y += bob;
    switch (thought) {
    case BABY_TANK_THOUGHT_STAR:
        fill_circle(center_x, center_y, s / 3, accent);
        draw_line(center_x - s, center_y, center_x + s, center_y, 1, accent);
        draw_line(center_x, center_y - s, center_x, center_y + s, 1, accent);
        draw_line(center_x - s * 2 / 3, center_y - s * 2 / 3,
                  center_x + s * 2 / 3, center_y + s * 2 / 3, 1, ink);
        draw_line(center_x + s * 2 / 3, center_y - s * 2 / 3,
                  center_x - s * 2 / 3, center_y + s * 2 / 3, 1, ink);
        break;
    case BABY_TANK_THOUGHT_MOON:
        fill_circle(center_x, center_y, s, accent);
        fill_circle(center_x + s / 2, center_y - s / 3, s, dark);
        break;
    case BABY_TANK_THOUGHT_CLOUD:
        fill_circle(center_x - s / 2, center_y, s / 2, ink);
        fill_circle(center_x, center_y - s / 3, s * 2 / 3, ink);
        fill_circle(center_x + s / 2, center_y, s / 2, ink);
        fill_rect(center_x - s, center_y, s * 2, s / 2, ink);
        break;
    case BABY_TANK_THOUGHT_HEART:
        fill_circle(center_x - s / 3, center_y - s / 4, s / 2, accent);
        fill_circle(center_x + s / 3, center_y - s / 4, s / 2, accent);
        for (int row = 0; row < s; ++row) {
            fill_rect(center_x - s + row, center_y - row / 3,
                      (s - row) * 2, 1, accent);
        }
        break;
    case BABY_TANK_THOUGHT_FLOWER:
        for (int index = 0; index < 4; ++index) {
            int dx = index == 0 ? -s / 2 : index == 1 ? s / 2 : 0;
            int dy = index == 2 ? -s / 2 : index == 3 ? s / 2 : 0;
            fill_circle(center_x + dx, center_y + dy, s / 2, ink);
        }
        fill_circle(center_x, center_y, s / 3, accent);
        break;
    case BABY_TANK_THOUGHT_ROCKET:
        fill_ellipse(center_x, center_y - 1, s / 2, s, ink);
        fill_circle(center_x, center_y - s / 3, 2, dark);
        draw_line(center_x - s / 2, center_y + s / 3,
                  center_x - s, center_y + s, 2, accent);
        draw_line(center_x + s / 2, center_y + s / 3,
                  center_x + s, center_y + s, 2, accent);
        draw_line(center_x, center_y + s, center_x, center_y + s + flap,
                  2, accent);
        break;
    case BABY_TANK_THOUGHT_CAR:
        fill_rect(center_x - s, center_y - s / 3, s * 2, s, ink);
        fill_rect(center_x - s / 2, center_y - s, s, s, ink);
        fill_circle(center_x - s / 2, center_y + s * 2 / 3, s / 3, dark);
        fill_circle(center_x + s / 2, center_y + s * 2 / 3, s / 3, dark);
        break;
    case BABY_TANK_THOUGHT_BALL:
        fill_circle(center_x, center_y, s, accent);
        draw_line(center_x - s, center_y, center_x + s, center_y, 1, dark);
        draw_line(center_x, center_y - s, center_x, center_y + s, 1, dark);
        break;
    case BABY_TANK_THOUGHT_FISH:
        fill_ellipse(center_x, center_y, s, s * 2 / 3, ink);
        draw_line(center_x - s, center_y, center_x - s * 3 / 2,
                  center_y - s * 2 / 3 + flap, 2, accent);
        draw_line(center_x - s, center_y, center_x - s * 3 / 2,
                  center_y + s * 2 / 3 - flap, 2, accent);
        fill_circle(center_x + s / 2, center_y - 1, 1, dark);
        break;
    case BABY_TANK_THOUGHT_BUBBLE:
        fill_circle(center_x - s / 2, center_y + s / 3, s / 2, ink);
        fill_circle(center_x + s / 3, center_y - s / 3, s * 2 / 3, accent);
        fill_circle(center_x + s, center_y + s / 2, s / 3, ink);
        break;
    case BABY_TANK_THOUGHT_QUESTION:
        draw_line(center_x - s / 2, center_y - s / 2,
                  center_x, center_y - s, 2, ink);
        draw_line(center_x, center_y - s, center_x + s / 2,
                  center_y - s / 2, 2, ink);
        draw_line(center_x + s / 2, center_y - s / 2,
                  center_x, center_y + s / 3, 2, ink);
        fill_circle(center_x, center_y + s, 2, accent);
        break;
    case BABY_TANK_THOUGHT_MUSIC:
        draw_line(center_x, center_y - s, center_x, center_y + s / 2,
                  2, ink);
        draw_line(center_x, center_y - s, center_x + s, center_y - s / 2,
                  2, ink);
        draw_line(center_x + s, center_y - s / 2,
                  center_x + s, center_y + s / 2, 2, ink);
        fill_circle(center_x - 2, center_y + s / 2, s / 3, accent);
        fill_circle(center_x + s - 2, center_y + s / 2, s / 3, accent);
        break;
    case BABY_TANK_THOUGHT_BIRD:
        fill_ellipse(center_x, center_y, s, s * 2 / 3, ink);
        draw_line(center_x - s / 2, center_y,
                  center_x, center_y - s / 2 + flap, 2, accent);
        draw_line(center_x, center_y - s / 2 + flap,
                  center_x + s / 3, center_y, 2, accent);
        draw_line(center_x + s, center_y, center_x + s + 4,
                  center_y + 2, 1, accent);
        fill_circle(center_x + s / 2, center_y - 2, 1, dark);
        break;
    case BABY_TANK_THOUGHT_BOOK:
        draw_line(center_x, center_y - s, center_x, center_y + s, 1, ink);
        draw_line(center_x, center_y - s, center_x - s, center_y - s / 2,
                  2, ink);
        draw_line(center_x - s, center_y - s / 2,
                  center_x - s, center_y + s, 2, ink);
        draw_line(center_x - s, center_y + s, center_x, center_y + s / 2,
                  2, ink);
        draw_line(center_x, center_y - s, center_x + s, center_y - s / 2,
                  2, accent);
        draw_line(center_x + s, center_y - s / 2,
                  center_x + s, center_y + s, 2, accent);
        draw_line(center_x + s, center_y + s, center_x, center_y + s / 2,
                  2, accent);
        break;
    case BABY_TANK_THOUGHT_KITE:
        draw_line(center_x, center_y - s, center_x + s, center_y, 2, accent);
        draw_line(center_x + s, center_y, center_x, center_y + s, 2, accent);
        draw_line(center_x, center_y + s, center_x - s, center_y, 2, ink);
        draw_line(center_x - s, center_y, center_x, center_y - s, 2, ink);
        draw_line(center_x, center_y + s, center_x + flap,
                  center_y + s * 2, 1, ink);
        break;
    default:
        break;
    }
}

static void draw_sleep_dream_story(uint32_t phase_ms,
                                   baby_tank_thought_t thought,
                                   const uint8_t *glyphs,
                                   size_t glyph_count,
                                   uint8_t content_mode,
                                   uint16_t black,
                                   uint16_t dream_line,
                                   uint16_t dream_text)
{
    /* Three separate breaths share one complete path.  The first two Zs
     * disappear at the upper-right endpoint; only the third survives there
     * and becomes the dream bubble.  Keeping the origin identical avoids
     * the former "three Zs placed along a diagonal" look. */
    /* The snore cue is visualized above the right closed eye rather than at
     * the mouth. Each breath follows the same gentle up-right path. */
    draw_travelling_sleep_z(phase_ms, 0U, 1500U,
                            180, 86, 194, 35, 7, false, dream_line);
    draw_travelling_sleep_z(phase_ms, 1650U, 1500U,
                            180, 86, 194, 35, 9, false, dream_line);
    draw_travelling_sleep_z(phase_ms, 3300U, 1600U,
                            180, 86, 194, 35, 12, true, dream_line);

    /*
     * Both contours have the same point count and direction. The open Z
     * stroke is morphed directly into this hand-shaped manga cloud outline;
     * no second cloud layer exists to fade in behind it.
     */
    static const int16_t z_points[28][2] = {
        {194,35},{196,35},{198,35},{200,35},{202,35},{204,35},{206,35},{208,35},
        {207,36},{206,37},{205,38},{204,39},{202,41},{201,42},{200,43},{199,44},
        {198,45},{197,46},{196,47},{194,49},{196,49},{198,49},{200,49},{202,49},
        {204,49},{206,49},{207,49},{208,49},
    };
    static const int16_t cloud_points[28][2] = {
        {145,55},{141,50},{142,44},{146,39},{152,37},{154,30},{160,25},{168,24},
        {175,28},{181,22},{189,20},{197,23},{201,29},{208,27},{215,30},{220,35},
        {221,41},{225,45},{225,52},{221,58},{214,61},{205,62},{196,63},{187,62},
        {178,64},{168,63},{158,62},{150,59},
    };
    if (phase_ms < 4900U) return;
    if (phase_ms >= 9500U) {
        draw_travelling_sleep_z(phase_ms, 9500U, 1300U,
                                194, 35, 216, 17, 12, false, dream_line);
        return;
    }

    float morph;
    if (phase_ms < 5900U) {
        morph = smoothstep((float)(phase_ms - 4900U) / 1000.0f);
    } else if (phase_ms < 8400U) {
        morph = 1.0f;
    } else {
        morph = 1.0f - smoothstep((float)(phase_ms - 8400U) / 1100.0f);
    }
    float settle = phase_ms > 5900U && phase_ms < 8400U
        ? smoothstep((float)(phase_ms - 5900U) / 500.0f) : 0.0f;
    uint32_t idle_ms = phase_ms > 5900U ? phase_ms - 5900U : 0U;
    int bob = rounded_float((float)smooth_wave(idle_ms, 3000U, 1) * settle);
    int breathe = rounded_float((float)smooth_wave(idle_ms, 3600U, 12) *
                                settle);
    int scale_1000 = 1000 + breathe;
    uint8_t alpha = 15U;
    uint16_t outline = thought_alpha_color(dream_line, alpha);
    uint16_t feather = thought_alpha_color(dream_line,
        (uint8_t)((alpha + 2U) / 3U));
    int px[28];
    int py[28];
    const int center_x = 184;
    const int center_y = 45;
    for (unsigned index = 0; index < 28U; ++index) {
        int target_x = center_x +
            (cloud_points[index][0] - center_x) * scale_1000 / 1000;
        int target_y = center_y +
            (cloud_points[index][1] - center_y) * scale_1000 / 1000 + bob;
        px[index] = z_points[index][0] + rounded_float(
            (float)(target_x - z_points[index][0]) * morph);
        py[index] = z_points[index][1] + rounded_float(
            (float)(target_y - z_points[index][1]) * morph);
    }
    for (unsigned index = 1; index < 28U; ++index) {
        draw_line(px[index - 1U], py[index - 1U], px[index], py[index],
                  4, feather);
    }
    for (unsigned index = 1; index < 28U; ++index) {
        draw_line(px[index - 1U], py[index - 1U], px[index], py[index],
                  2, outline);
    }
    if (morph > 0.58f) {
        float close = smoothstep((morph - 0.58f) / 0.42f);
        int close_x = px[27] + rounded_float((float)(px[0] - px[27]) * close);
        int close_y = py[27] + rounded_float((float)(py[0] - py[27]) * close);
        draw_line(px[27], py[27], close_x, close_y, 4, feather);
        draw_line(px[27], py[27], close_x, close_y, 2, outline);
    }

    if (morph > 0.58f) {
        int text_scale = rounded_float(morph * 1000.0f);
        uint8_t text_alpha = (uint8_t)rounded_float(morph * 15.0f);
        uint16_t visible_text = thought_alpha_color(dream_text, text_alpha);
        uint16_t accent = thought_alpha_color(rgb565(241, 185, 72), text_alpha);
        if (content_mode == 0U) {
            draw_thought_text_scaled(glyphs, glyph_count, center_x, 46 + bob,
                                     text_scale, visible_text,
                                     phase_ms - 4900U);
        } else if (content_mode == 1U) {
            draw_dream_icon(thought, center_x, 45 + bob, 10,
                            idle_ms, visible_text, accent, black);
        } else {
            draw_dream_icon(thought, center_x - 18, 45 + bob, 7,
                            idle_ms, visible_text, accent, black);
            draw_thought_text_scaled(glyphs, glyph_count, center_x + 17,
                                     46 + bob, text_scale * 560 / 1000,
                                     visible_text, phase_ms - 4900U);
        }
    }
    (void)black;
}

static void draw_sleep_thought_bubble(uint32_t elapsed_ms)
{
    baby_tank_thought_t thought;
    uint8_t glyphs[4];
    size_t glyph_count;
    taskENTER_CRITICAL(&s_thought_lock);
    size_t deck_count = s_dream_deck_count;
    uint32_t deck_slot = deck_count > 0U
        ? (elapsed_ms / 12000U) % deck_count : 0U;
    if (deck_count > 0U) {
        thought = s_dream_deck[deck_slot].thought;
        memcpy(glyphs, s_dream_deck[deck_slot].glyphs, sizeof(glyphs));
        glyph_count = s_dream_deck[deck_slot].glyph_count;
    } else {
        thought = s_sleep_thought;
        memcpy(glyphs, s_sleep_glyphs, sizeof(glyphs));
        glyph_count = s_sleep_glyph_count;
    }
    taskEXIT_CRITICAL(&s_thought_lock);
    if (thought == BABY_TANK_THOUGHT_NONE || glyph_count == 0U) return;

    /* The cloud mood often keeps the same cached "软绵绵" caption for hours.
     * Show that approved/cloud caption first, then rotate gentle local dream
     * fragments once a minute. This adds no model call, NVS write or buffer. */
    static const baby_tank_thought_t alternatives[] = {
        BABY_TANK_THOUGHT_MOON,
        BABY_TANK_THOUGHT_FLOWER,
        BABY_TANK_THOUGHT_FISH,
        BABY_TANK_THOUGHT_STAR,
        BABY_TANK_THOUGHT_MUSIC,
    };
    uint32_t caption_slot = deck_count > 0U ? 0U : (elapsed_ms / 60000U) %
        (1U + sizeof(alternatives) / sizeof(alternatives[0]));
    if (caption_slot != 0U) {
        baby_tank_thought_t selected = alternatives[caption_slot - 1U];
        glyph_count = thought_caption_count(selected);
        memcpy(glyphs, s_thought_captions[selected], sizeof(glyphs));
    }

    const uint16_t black = rgb565(0, 0, 0);
    const uint16_t dream_line = rgb565(70, 150, 143);
    const uint16_t dream_text = rgb565(116, 190, 177);
    s_canvas_offset_x = 0;
    s_canvas_offset_y = 0;
    uint32_t phase_ms = elapsed_ms % 12000U;

    uint8_t content_mode = deck_count > 0U ? (uint8_t)(deck_slot % 3U) : 0U;
    draw_sleep_dream_story(phase_ms, thought, glyphs, glyph_count,
                           content_mode, black,
                           dream_line, dream_text);
}

esp_err_t baby_tank_display_render_expression(baby_tank_expression_t expression,
                                              uint32_t elapsed_ms,
                                              uint32_t duration_ms,
                                              uint8_t intensity,
                                              uint8_t speech_level)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL && s_framebuffer != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "display is not initialized");
    ESP_RETURN_ON_FALSE(expression >= 0 && expression < BABY_TANK_EXPRESSION_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid expression");
    ESP_RETURN_ON_ERROR(wait_for_frame(), TAG, "previous frame is still pending");

    draw_expression(expression, elapsed_ms, duration_ms, intensity, speech_level);
    if (expression == BABY_TANK_EXPRESSION_SLEEPING) {
        draw_sleep_thought_bubble(elapsed_ms);
    }
    if (expression != BABY_TANK_EXPRESSION_SLEEPING) {
        draw_status_line();
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel,
                                                  0,
                                                  0,
                                                  LCD_WIDTH,
                                                  LCD_HEIGHT,
                                                  s_framebuffer),
                        TAG, "frame transfer failed");
    s_transfer_pending = true;

    if (!s_backlight_on) {
        ESP_RETURN_ON_ERROR(wait_for_frame(), TAG, "first frame transfer failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_BACKLIGHT,
                                           LCD_BACKLIGHT_ON_LEVEL),
                            TAG, "backlight enable failed");
        s_backlight_on = true;
    }
    return ESP_OK;
}

esp_err_t baby_tank_display_render_drawing(const baby_tank_drawing_t *drawing)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL && s_framebuffer != NULL &&
                            drawing != NULL && drawing->pixels != NULL &&
                            drawing->width == LCD_WIDTH &&
                            drawing->height == LCD_HEIGHT &&
                            drawing->pixel_count == LCD_WIDTH * LCD_HEIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "drawing unavailable");
    ESP_RETURN_ON_ERROR(wait_for_frame(), TAG, "previous frame pending");
    s_canvas_offset_x = 0;
    s_canvas_offset_y = 0;
    memcpy(s_framebuffer, drawing->pixels,
           drawing->pixel_count * sizeof(*drawing->pixels));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                         LCD_WIDTH, LCD_HEIGHT, s_framebuffer),
                        TAG, "drawing transfer failed");
    s_transfer_pending = true;
    if (!s_backlight_on) {
        ESP_RETURN_ON_ERROR(wait_for_frame(), TAG,
                            "first drawing transfer failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_BACKLIGHT,
                            LCD_BACKLIGHT_ON_LEVEL), TAG,
                            "drawing backlight failed");
        s_backlight_on = true;
    }
    return ESP_OK;
}

/* Six tiny idle stories share one 120x120 logical grid. Every art pixel maps
 * to exactly 2x2 LCD pixels: no interpolation, second framebuffer or JPEG. */
static void scene_box(int x, int y, int width, int height, uint16_t color)
{
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > 120 ? 120 : x + width;
    int bottom = y + height > 120 ? 120 : y + height;
    for (int row = top; row < bottom; ++row) {
        for (int column = left; column < right; ++column) {
            int physical = row * 2 * LCD_WIDTH + column * 2;
            s_framebuffer[physical] = color;
            s_framebuffer[physical + 1] = color;
            s_framebuffer[physical + LCD_WIDTH] = color;
            s_framebuffer[physical + LCD_WIDTH + 1] = color;
        }
    }
}

typedef struct {
    uint16_t ink;
    uint16_t sky;
    uint16_t floor;
    uint16_t ambient;
    uint16_t white;
    uint16_t shadow;
    uint16_t cyan;
    uint16_t orange;
    uint16_t green;
    uint16_t pale;
    uint16_t label;
} pixel_palette_t;

static pixel_palette_t scene_palette(baby_tank_scene_t scene)
{
    pixel_palette_t p = {
        .ink = rgb565(13, 29, 37),
        .sky = rgb565(22, 46, 57),
        .floor = rgb565(37, 65, 71),
        .ambient = rgb565(51, 91, 101),
        .white = rgb565(245, 245, 229),
        .shadow = rgb565(161, 177, 170),
        .cyan = rgb565(62, 217, 220),
        .orange = rgb565(242, 176, 100),
        .green = rgb565(126, 190, 151),
        .pale = rgb565(110, 150, 157),
        .label = rgb565(110, 150, 157),
    };
    /* Outdoor stories run only in daytime: use a recognizable blue sky,
     * while indoor stories keep subdued wall colors. Shared accent ramps
     * keep the six scenes and the white character visually coherent. */
    switch (scene) {
    case BABY_TANK_SCENE_EXERCISE:
        p.sky = rgb565(61, 142, 194);
        p.floor = rgb565(59, 104, 95);
        p.ambient = rgb565(112, 173, 169);
        p.pale = rgb565(192, 226, 227);
        p.label = rgb565(24, 72, 101);
        break;
    case BABY_TANK_SCENE_READING:
        p.sky = rgb565(53, 48, 63);
        p.floor = rgb565(72, 63, 77);
        p.ambient = rgb565(91, 76, 91);
        break;
    case BABY_TANK_SCENE_BLOCKS:
        p.sky = rgb565(36, 56, 72);
        p.floor = rgb565(56, 76, 85);
        p.ambient = rgb565(76, 98, 106);
        break;
    case BABY_TANK_SCENE_EXPLORE:
        p.sky = rgb565(54, 136, 187);
        p.floor = rgb565(57, 108, 85);
        p.ambient = rgb565(93, 158, 143);
        p.pale = rgb565(185, 219, 219);
        p.label = rgb565(22, 69, 97);
        break;
    case BABY_TANK_SCENE_CLOUDS:
        p.sky = rgb565(55, 145, 202);
        p.floor = rgb565(65, 114, 83);
        p.ambient = rgb565(109, 170, 161);
        p.pale = rgb565(188, 222, 232);
        p.label = rgb565(20, 68, 101);
        break;
    case BABY_TANK_SCENE_TIDY:
        p.sky = rgb565(49, 55, 67);
        p.floor = rgb565(69, 73, 80);
        p.ambient = rgb565(89, 91, 96);
        break;
    default: break;
    }
    return p;
}

static void scene_tank(int x, int y, unsigned frame,
                       baby_tank_scene_t scene,
                       const pixel_palette_t *p)
{
    int bob = frame == 1U ? -1 : 0;
    /* Smaller, recognizable white head / wedge / black treads. The props
     * now own the foreground instead of being crowded out by the mascot. */
    scene_box(x, y + 30, 9, 25, p->ink);
    scene_box(x + 33, y + 30, 9, 25, p->ink);
    scene_box(x + 2, y + 33, 5, 19, p->floor);
    scene_box(x + 35, y + 33, 5, 19, p->floor);
    for (int row = 0; row < 4; ++row) {
        int tread_y = y + 35 + row * 4 + (int)(frame & 1U);
        scene_box(x + 2, tread_y, 5, 1, p->ink);
        scene_box(x + 35, tread_y, 5, 1, p->ink);
    }
    scene_box(x + 8, y + 29 + bob, 27, 23, p->shadow);
    scene_box(x + 10, y + 28 + bob, 24, 22, p->white);
    scene_box(x + 13, y + 49 + bob, 21, 2, p->ink);
    scene_box(x + 10, y + 5 + bob, 23, 27, p->shadow);
    scene_box(x + 9, y + 4 + bob, 23, 26, p->white);
    scene_box(x + 12, y + 8 + bob, 17, 18, p->ink);
    int eyes_y = scene == BABY_TANK_SCENE_CLOUDS ? 12 :
                 scene == BABY_TANK_SCENE_READING ? 17 : 14;
    scene_box(x + 14, y + eyes_y + bob, 4, 1, p->cyan);
    scene_box(x + 23, y + eyes_y + bob, 4, 1, p->cyan);
    scene_box(x + 19, y + 20 + bob, 3, 1, p->cyan);
    scene_box(x + 18, y + 2 + bob, 6, 2, p->white);
    scene_box(x + 20, y + 1 + bob, 2, 1, p->ink);
}

static void scene_cloud(int x, int y, const pixel_palette_t *p)
{
    scene_box(x + 4, y, 11, 3, p->white);
    scene_box(x, y + 3, 24, 5, p->white);
    scene_box(x + 7, y + 8, 12, 2, p->pale);
}

static void scene_background(baby_tank_scene_t scene, unsigned frame,
                             const pixel_palette_t *p)
{
    scene_box(0, 0, 120, 120, p->sky);
    scene_box(0, 86, 120, 34, p->floor);
    scene_box(0, 86, 120, 2, p->ambient);
    switch (scene) {
    case BABY_TANK_SCENE_EXERCISE:
        /* Early light, running-track stripes, and a clear exercise mat. */
        scene_box(0, 67, 120, 19, p->ambient);
        scene_box(7, 15, 16, 16, p->orange);
        scene_box(11, 19, 8, 8, p->sky);
        scene_box(0, 72, 28, 2, p->pale);
        scene_box(94, 72, 26, 2, p->pale);
        scene_box(12, 101, 96, 7, p->ink);
        scene_box(17, 103, 86, 3, p->cyan);
        break;
    case BABY_TANK_SCENE_READING:
        /* A quiet room: two shelves and a warm reading lamp. */
        scene_box(4, 29, 22, 56, p->ink);
        scene_box(6, 31, 18, 4, p->ambient);
        scene_box(7, 38, 4, 13, p->orange);
        scene_box(13, 42, 5, 9, p->pale);
        scene_box(20, 36, 3, 15, p->green);
        scene_box(6, 54, 18, 3, p->ambient);
        scene_box(9, 58, 5, 16, p->green);
        scene_box(16, 61, 5, 13, p->orange);
        scene_box(94, 39, 2, 43, p->shadow);
        scene_box(87, 38, 16, 4, p->shadow);
        scene_box(90, 43, 11, 7, p->orange);
        scene_box(93, 48, 5, 2, p->white);
        scene_box(17, 101, 87, 5, p->ambient);
        break;
    case BABY_TANK_SCENE_BLOCKS:
        /* Broad play mat and a pair of simple wall pennants. */
        scene_box(0, 30, 120, 2, p->ambient);
        scene_box(11, 31, 12, 14, p->orange);
        scene_box(15, 45, 4, 4, p->orange);
        scene_box(98, 31, 12, 14, p->green);
        scene_box(102, 45, 4, 4, p->green);
        scene_box(9, 101, 102, 7, p->ink);
        scene_box(13, 103, 94, 3, p->ambient);
        break;
    case BABY_TANK_SCENE_EXPLORE:
        /* Layered distant hills and a trail; neither moves the chassis. */
        scene_box(0, 55, 21, 31, p->ambient);
        scene_box(8, 49, 19, 37, p->ambient);
        scene_box(94, 51, 26, 35, p->ambient);
        scene_box(102, 45, 18, 41, p->ambient);
        scene_box(0, 81, 120, 6, p->green);
        scene_box(6, 101, 36, 2, p->pale);
        scene_box(78, 105, 36, 2, p->pale);
        break;
    case BABY_TANK_SCENE_CLOUDS:
        /* Large sky shapes carry the story; no noisy pixel texture. */
        scene_box(0, 70, 120, 16, p->ambient);
        scene_box(0, 76, 26, 10, p->green);
        scene_box(92, 73, 28, 13, p->green);
        scene_cloud(4 + (int)(frame & 1U), 14, p);
        scene_cloud(91 - (int)(frame & 1U), 31, p);
        break;
    case BABY_TANK_SCENE_TIDY:
        /* Side shelves make the storage action readable as a room. */
        scene_box(1, 29, 28, 55, p->ink);
        scene_box(3, 31, 24, 4, p->ambient);
        scene_box(5, 39, 6, 17, p->green);
        scene_box(13, 43, 7, 13, p->orange);
        scene_box(22, 39, 3, 17, p->pale);
        scene_box(3, 58, 24, 4, p->ambient);
        scene_box(95, 31, 22, 54, p->ambient);
        scene_box(98, 35, 16, 2, p->shadow);
        scene_box(98, 58, 16, 2, p->shadow);
        scene_box(15, 101, 90, 2, p->pale);
        break;
    default: break;
    }
}

/* A quiet 10x10 status line: deliberately smaller and dimmer than the art.
 * One logical pixel is still exactly 2x2 LCD pixels, with no smoothing. */
static const uint16_t s_scene_label_glyphs[][10] = {
    /* 做 */ {0x0a4,0x0a0,0x1ff,0x1aa,0x1fa,0x0d4,0x0f4,0x0ce,0x092,0x000},
    /* 操 */ {0x0bc,0x0a4,0x1fc,0x0b6,0x0f6,0x180,0x0fe,0x0ac,0x14a,0x000},
    /* 看 */ {0x1fe,0x020,0x0fc,0x1ff,0x07c,0x1fc,0x17c,0x044,0x07c,0x000},
    /* 书 */ {0x020,0x026,0x1fc,0x024,0x020,0x1fe,0x022,0x024,0x020,0x000},
    /* 积 */ {0x0de,0x192,0x092,0x1d2,0x09e,0x1c0,0x194,0x092,0x0a0,0x000},
    /* 木 */ {0x010,0x010,0x1fe,0x030,0x078,0x0d8,0x094,0x112,0x030,0x000},
    /* 冒 */ {0x000,0x1fe,0x17a,0x17a,0x0fc,0x0fc,0x0fc,0x084,0x0fc,0x000},
    /* 险 */ {0x010,0x198,0x0a6,0x0fe,0x180,0x0b4,0x0bc,0x188,0x37e,0x000},
    /* 云 */ {0x000,0x0fc,0x000,0x1fe,0x020,0x048,0x0c4,0x1fe,0x000,0x000},
    /* 整 */ {0x048,0x1ee,0x1f8,0x1ec,0x0ea,0x1fc,0x01c,0x080,0x1fe,0x000},
    /* 理 */ {0x03e,0x1ea,0x0be,0x1ea,0x0be,0x088,0x0be,0x188,0x07f,0x000},
    /* 中 */ {0x030,0x020,0x1fe,0x122,0x122,0x1fe,0x120,0x020,0x030,0x000},
};

static void scene_label_glyph(unsigned glyph, int x, int y, uint16_t color)
{
    for (int row = 0; row < 10; ++row) {
        uint16_t bits = s_scene_label_glyphs[glyph][row];
        for (int column = 0; column < 10; ++column) {
            if ((bits & (uint16_t)(0x0200U >> column)) != 0U) {
                scene_box(x + column, y + row, 1, 1, color);
            }
        }
    }
}

static void scene_label(baby_tank_scene_t scene, const pixel_palette_t *p)
{
    /* 做操中… / 看书中… / 积木中… / 冒险中… / 看云中… / 整理中… */
    static const uint8_t pairs[BABY_TANK_SCENE_COUNT][2] = {
        [BABY_TANK_SCENE_EXERCISE] = {0, 1},
        [BABY_TANK_SCENE_READING] = {2, 3},
        [BABY_TANK_SCENE_BLOCKS] = {4, 5},
        [BABY_TANK_SCENE_EXPLORE] = {6, 7},
        [BABY_TANK_SCENE_CLOUDS] = {2, 8},
        [BABY_TANK_SCENE_TIDY] = {9, 10},
    };
    scene_label_glyph(pairs[scene][0], 78, 5, p->label);
    scene_label_glyph(pairs[scene][1], 89, 5, p->label);
    scene_label_glyph(11, 100, 5, p->label);
    scene_box(112, 13, 1, 1, p->label);
    scene_box(115, 13, 1, 1, p->label);
    scene_box(118, 13, 1, 1, p->label);
}

static void scene_props(baby_tank_scene_t scene, unsigned frame,
                        const pixel_palette_t *p)
{
    switch (scene) {
    case BABY_TANK_SCENE_EXERCISE:
        /* Lift alternates a few pixels; large weights explain the action. */
        scene_box(27, 78 - (int)(frame & 1U) * 4, 66, 4, p->shadow);
        scene_box(24, 68 - (int)(frame & 1U) * 4, 8, 23, p->cyan);
        scene_box(33, 72 - (int)(frame & 1U) * 4, 5, 15, p->cyan);
        scene_box(82, 72 - (int)(frame & 1U) * 4, 5, 15, p->cyan);
        scene_box(88, 68 - (int)(frame & 1U) * 4, 8, 23, p->cyan);
        scene_box(19, 53, 3, 10, p->orange);
        scene_box(14, 57, 13, 3, p->orange);
        scene_box(99, 53, 3, 10, p->orange);
        scene_box(94, 57, 13, 3, p->orange);
        break;
    case BABY_TANK_SCENE_READING:
        /* Open book overlaps the chest instead of hiding behind it. */
        scene_box(29, 75, 62, 24, p->ink);
        scene_box(31, 73, 28, 23, p->orange);
        scene_box(60, 73, 28, 23, p->white);
        scene_box(59, 75, 2, 20, p->shadow);
        scene_box(36, 80, 17, 2, p->ink);
        scene_box(36, 86, 15, 2, p->ink);
        scene_box(66, 80, 17, 2, p->pale);
        scene_box(66, 86, 13, 2, p->pale);
        if (frame == 3U) scene_box(87, 66, 4, 6, p->cyan);
        break;
    case BABY_TANK_SCENE_BLOCKS:
        scene_box(8, 72, 23, 23, p->orange);
        scene_box(11, 75, 17, 3, p->white);
        scene_box(89, 72, 23, 23, p->green);
        scene_box(92, 75, 17, 3, p->white);
        scene_box(76, 49 - (int)(frame == 2U) * 5, 22, 22, p->cyan);
        scene_box(79, 52 - (int)(frame == 2U) * 5, 16, 3, p->white);
        scene_box(78, 68, 3, 5, p->pale);
        break;
    case BABY_TANK_SCENE_EXPLORE:
        /* Folded map with a bold route and a compass marker. */
        scene_box(25, 73, 70, 27, p->ink);
        scene_box(28, 75, 20, 22, p->green);
        scene_box(49, 75, 20, 22, p->white);
        scene_box(70, 75, 22, 22, p->green);
        scene_box(48, 75, 2, 22, p->shadow);
        scene_box(69, 75, 2, 22, p->shadow);
        scene_box(36, 87, 17, 3, p->orange);
        scene_box(52, 82, 18, 3, p->orange);
        scene_box(68, 80, 13, 3, p->orange);
        scene_box(80, 76, 5, 8, p->cyan);
        scene_box(102, 24, 3, 20, p->cyan);
        scene_box(94, 32, 19, 3, p->cyan);
        break;
    case BABY_TANK_SCENE_CLOUDS:
        scene_box(8, 75, 3, 24, p->green);
        scene_box(1, 69, 17, 9, p->orange);
        scene_box(6, 71, 4, 4, p->white);
        scene_box(105, 76, 3, 23, p->green);
        scene_box(98, 70, 17, 9, p->cyan);
        scene_box(103, 72, 4, 4, p->white);
        break;
    case BABY_TANK_SCENE_TIDY:
        scene_box(6, 72, 31, 25, p->orange);
        scene_box(10, 68, 23, 6, p->white);
        scene_box(12, 80, 19, 3, p->ink);
        scene_box(94, 56, 4, 42, p->shadow);
        scene_box(86, 90, 21, 7, p->green);
        scene_box(77, 68, 18, 9, p->cyan);
        scene_box(80, 71, 12, 3, p->white);
        scene_box(107, 78 - (int)(frame & 1U) * 3, 5, 5, p->pale);
        break;
    default: break;
    }
}

esp_err_t baby_tank_display_render_scene(baby_tank_scene_t scene,
                                         uint32_t elapsed_ms)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL && s_framebuffer != NULL &&
                            scene >= 0 && scene < BABY_TANK_SCENE_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "pixel scene unavailable");
    ESP_RETURN_ON_ERROR(wait_for_frame(), TAG, "previous scene frame pending");
    pixel_palette_t p = scene_palette(scene);
    unsigned frame = (elapsed_ms / 900U) % 4U;
    scene_background(scene, frame, &p);
    scene_tank(39, 32, frame, scene, &p);
    scene_props(scene, frame, &p);
    scene_label(scene, &p);
    draw_status_line();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
        LCD_WIDTH, LCD_HEIGHT, s_framebuffer), TAG, "scene transfer failed");
    s_transfer_pending = true;
    if (!s_backlight_on) {
        ESP_RETURN_ON_ERROR(wait_for_frame(), TAG, "first scene frame failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_BACKLIGHT,
            LCD_BACKLIGHT_ON_LEVEL), TAG, "scene backlight failed");
        s_backlight_on = true;
    }
    return ESP_OK;
}
