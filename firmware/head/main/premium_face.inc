/*
 * Baby Tank face performance renderer.
 *
 * Kept in a separate include so the approved character language and its
 * timing model stay isolated from the ST7789 transport bring-up code.
 */

typedef struct {
    float width;
    float height;
    float offset_x;
    float offset_y;
    float outer_lid;
    float inner_lid;
    float smile_cut;
} eye_target_values_t;

typedef struct {
    float width;
    float curve;
    float open_height;
    float offset_x;
    float offset_y;
} mouth_target_values_t;

typedef struct {
    eye_target_values_t left;
    eye_target_values_t right;
    mouth_target_values_t mouth;
} face_target_values_t;

#define EYE(w, h, x, y, outer, inner, smile) \
    { (w), (h), (x), (y), (outer), (inner), (smile) }
#define MOUTH(w, curve, open, x, y) { (w), (curve), (open), (x), (y) }

static const face_target_values_t s_face_targets[BABY_TANK_EXPRESSION_COUNT] = {
    [BABY_TANK_EXPRESSION_CALM] =
        {EYE(32,40,0,0,0,0,0), EYE(32,40,0,0,0,0,0), MOUTH(25,0,7,0,0)},
    [BABY_TANK_EXPRESSION_HAPPY] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(34,10,0,0,0)},
    [BABY_TANK_EXPRESSION_LAUGHING] =
        {EYE(36,21,0,0,0,0,14), EYE(36,21,0,0,0,0,14), MOUTH(32,0,20,0,3)},
    [BABY_TANK_EXPRESSION_EXCITED] =
        {EYE(37,47,0,-2,0,0,0), EYE(37,47,0,-2,0,0,0), MOUTH(26,0,15,0,2)},
    [BABY_TANK_EXPRESSION_LOVE] =
        {EYE(35,43,1,-1,0,0,0), EYE(35,43,-1,-1,0,0,0), MOUTH(32,9,0,0,0)},
    [BABY_TANK_EXPRESSION_CURIOUS] =
        {EYE(37,45,-1,-2,0,0,0), EYE(28,34,1,2,0,0,0), MOUTH(18,0,9,5,1)},
    [BABY_TANK_EXPRESSION_SURPRISED] =
        {EYE(37,49,0,-2,0,0,0), EYE(37,49,0,-2,0,0,0), MOUTH(18,0,16,0,3)},
    [BABY_TANK_EXPRESSION_SHY] =
        {EYE(34,25,2,2,0,0,10), EYE(34,25,-2,2,0,0,10), MOUTH(25,6,0,-3,1)},
    [BABY_TANK_EXPRESSION_PROUD] =
        {EYE(34,34,0,0,2,9,0), EYE(34,34,0,0,2,9,0), MOUTH(27,4,0,5,0)},
    [BABY_TANK_EXPRESSION_BORED] =
        {EYE(34,22,0,3,4,4,0), EYE(34,22,0,3,4,4,0), MOUTH(29,0,6,0,3)},
    [BABY_TANK_EXPRESSION_ANNOYED] =
        {EYE(35,31,0,1,2,15,0), EYE(35,31,0,1,2,15,0), MOUTH(30,-3,0,4,2)},
    [BABY_TANK_EXPRESSION_COMPLAINING] =
        {EYE(34,18,0,2,2,4,8), EYE(32,39,0,0,0,5,0), MOUTH(38,0,11,-4,3)},
    [BABY_TANK_EXPRESSION_ANGRY] =
        {EYE(36,34,0,1,2,18,0), EYE(36,34,0,1,2,18,0), MOUTH(36,-10,0,0,5)},
    [BABY_TANK_EXPRESSION_FURIOUS] =
        {EYE(38,30,-2,1,2,21,0), EYE(38,30,2,1,2,21,0), MOUTH(40,-12,0,0,5)},
    [BABY_TANK_EXPRESSION_SAD] =
        {EYE(34,33,0,2,16,2,0), EYE(34,33,0,2,16,2,0), MOUTH(35,-10,0,0,5)},
    [BABY_TANK_EXPRESSION_CRYING] =
        {EYE(34,29,0,1,17,2,4), EYE(34,29,0,1,17,2,4), MOUTH(36,-11,0,0,6)},
    [BABY_TANK_EXPRESSION_WORRIED] =
        {EYE(31,36,-1,1,13,1,0), EYE(34,39,1,0,13,1,0), MOUTH(29,-6,0,2,3)},
    [BABY_TANK_EXPRESSION_CONFUSED] =
        {EYE(38,44,-1,-2,0,0,0), EYE(30,24,1,3,3,7,5), MOUTH(28,-4,0,5,1)},
    [BABY_TANK_EXPRESSION_DIZZY] =
        {EYE(36,38,-2,0,0,0,0), EYE(36,38,2,0,0,0,0), MOUTH(18,0,10,0,3)},
    [BABY_TANK_EXPRESSION_SICK] =
        {EYE(34,27,0,3,8,8,3), EYE(34,27,0,3,8,8,3), MOUTH(34,-5,0,0,4)},
    [BABY_TANK_EXPRESSION_THINKING] =
        {EYE(32,39,-1,0,0,3,0), EYE(32,39,-1,0,0,3,0), MOUTH(24,-3,0,7,1)},
    [BABY_TANK_EXPRESSION_SLEEPY] =
        {EYE(35,15,0,3,4,4,4), EYE(35,15,0,3,4,4,4), MOUTH(14,0,9,0,4)},
    [BABY_TANK_EXPRESSION_SLEEPING] =
        {EYE(36,18,0,3,0,0,12), EYE(36,18,0,3,0,0,12), MOUTH(23,0,15,-1,7)},
    [BABY_TANK_EXPRESSION_BUBBLE] =
        {EYE(36,18,0,3,0,0,12), EYE(36,18,0,3,0,0,12), MOUTH(22,0,14,-1,7)},
    [BABY_TANK_EXPRESSION_CANNON] =
        {EYE(39,18,0,2,1,1,12), EYE(39,18,0,2,1,1,12), MOUTH(22,0,12,0,3)},
    [BABY_TANK_EXPRESSION_LISTENING] =
        {EYE(35,46,2,-2,0,0,0), EYE(35,46,2,-2,0,0,0), MOUTH(18,0,7,0,1)},
    [BABY_TANK_EXPRESSION_PROCESSING] =
        {EYE(32,38,3,-3,0,4,0), EYE(32,38,3,-3,0,4,0), MOUTH(24,-3,0,6,1)},
    [BABY_TANK_EXPRESSION_EATING] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_DRINKING] =
        {EYE(35,28,0,1,0,0,9), EYE(35,28,0,1,0,0,9), MOUTH(18,0,9,4,2)},
    [BABY_TANK_EXPRESSION_ROCK] =
        {EYE(35,42,0,-1,0,0,0), EYE(35,42,0,-1,0,0,0), MOUTH(24,4,0,0,1)},
    [BABY_TANK_EXPRESSION_PAPER] =
        {EYE(35,42,0,-1,0,0,0), EYE(35,42,0,-1,0,0,0), MOUTH(24,4,0,0,1)},
    [BABY_TANK_EXPRESSION_SCISSORS] =
        {EYE(35,42,0,-1,0,0,0), EYE(35,42,0,-1,0,0,0), MOUTH(24,4,0,0,1)},
    [BABY_TANK_EXPRESSION_FIGHTING] =
        {EYE(38,28,-1,0,2,15,0), EYE(38,28,1,0,2,15,0), MOUTH(34,-8,0,0,4)},
    [BABY_TANK_EXPRESSION_DICE_1] =
        {EYE(36,45,0,-2,0,0,0), EYE(36,45,0,-2,0,0,0), MOUTH(20,0,10,0,2)},
    [BABY_TANK_EXPRESSION_DICE_2] =
        {EYE(36,45,0,-2,0,0,0), EYE(36,45,0,-2,0,0,0), MOUTH(20,0,10,0,2)},
    [BABY_TANK_EXPRESSION_DICE_3] =
        {EYE(36,45,0,-2,0,0,0), EYE(36,45,0,-2,0,0,0), MOUTH(20,0,10,0,2)},
    [BABY_TANK_EXPRESSION_DICE_4] =
        {EYE(36,45,0,-2,0,0,0), EYE(36,45,0,-2,0,0,0), MOUTH(20,0,10,0,2)},
    [BABY_TANK_EXPRESSION_DICE_5] =
        {EYE(36,45,0,-2,0,0,0), EYE(36,45,0,-2,0,0,0), MOUTH(20,0,10,0,2)},
    [BABY_TANK_EXPRESSION_DICE_6] =
        {EYE(36,45,0,-2,0,0,0), EYE(36,45,0,-2,0,0,0), MOUTH(20,0,10,0,2)},
    [BABY_TANK_EXPRESSION_STRAWBERRY] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_APPLE] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_BANANA] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_CAKE] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_COOKIE] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_BREAD] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_RICE_BALL] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_ICE_CREAM] =
        {EYE(34,23,0,1,0,0,12), EYE(34,23,0,1,0,0,12), MOUTH(25,0,17,0,3)},
    [BABY_TANK_EXPRESSION_MILK] =
        {EYE(35,28,0,1,0,0,9), EYE(35,28,0,1,0,0,9), MOUTH(18,0,9,4,2)},
    [BABY_TANK_EXPRESSION_JUICE] =
        {EYE(35,28,0,1,0,0,9), EYE(35,28,0,1,0,0,9), MOUTH(18,0,9,4,2)},
    [BABY_TANK_EXPRESSION_WATER] =
        {EYE(35,28,0,1,0,0,9), EYE(35,28,0,1,0,0,9), MOUTH(18,0,9,4,2)},
    [BABY_TANK_EXPRESSION_YOGURT] =
        {EYE(35,28,0,1,0,0,9), EYE(35,28,0,1,0,0,9), MOUTH(18,0,9,4,2)},
    [BABY_TANK_EXPRESSION_SOY_MILK] =
        {EYE(35,28,0,1,0,0,9), EYE(35,28,0,1,0,0,9), MOUTH(18,0,9,4,2)},
    [BABY_TANK_EXPRESSION_COCOA] =
        {EYE(35,28,0,1,0,0,9), EYE(35,28,0,1,0,0,9), MOUTH(18,0,9,4,2)},
};

#undef EYE
#undef MOUTH

static void premium_set_eye_target(eye_pose_t *eye, const eye_target_values_t *target)
{
    eye->width.target = target->width;
    eye->height.target = target->height;
    eye->offset_x.target = target->offset_x;
    eye->offset_y.target = target->offset_y;
    eye->outer_lid.target = target->outer_lid;
    eye->inner_lid.target = target->inner_lid;
    eye->smile_cut.target = target->smile_cut;
}

static void premium_configure_targets(baby_tank_expression_t expression)
{
    const face_target_values_t *target = &s_face_targets[expression];
    premium_set_eye_target(&s_left_eye, &target->left);
    premium_set_eye_target(&s_right_eye, &target->right);
    s_mouth.width.target = target->mouth.width;
    s_mouth.curve.target = target->mouth.curve;
    s_mouth.open_height.target = target->mouth.open_height;
    s_mouth.offset_x.target = target->mouth.offset_x;
    s_mouth.offset_y.target = target->mouth.offset_y;
}

static float premium_interval(uint32_t time_ms, uint32_t start_ms, uint32_t end_ms)
{
    if (time_ms <= start_ms) return 0.0f;
    if (time_ms >= end_ms) return 1.0f;
    return smoothstep((float)(time_ms - start_ms) / (float)(end_ms - start_ms));
}

static uint32_t premium_exit_start(uint32_t duration_ms)
{
    uint32_t exit_start = (uint32_t)((uint64_t)duration_ms * 70U / 100U);
    return exit_start > 540U ? exit_start : 540U;
}

static float premium_story_weight(uint32_t elapsed_ms,
                                  uint32_t delay_ms,
                                  uint32_t duration_ms)
{
    if (elapsed_ms <= delay_ms + 90U) return 0.0f;
    uint32_t time_ms = elapsed_ms - delay_ms;
    if (time_ms < 270U) return premium_interval(time_ms, 90U, 270U) * 1.10f;
    if (time_ms < 540U) return 1.10f - premium_interval(time_ms, 270U, 540U) * 0.10f;
    uint32_t exit_start = premium_exit_start(duration_ms);
    uint32_t exit_end = (uint32_t)((uint64_t)duration_ms * 79U / 100U);
    if (exit_end <= exit_start) exit_end = exit_start + 1U;
    if (time_ms < exit_start) return 1.0f;
    if (time_ms < exit_end) {
        return 1.0f - premium_interval(time_ms, exit_start, exit_end);
    }
    return 0.0f;
}

static float premium_anticipation(uint32_t elapsed_ms, uint32_t delay_ms)
{
    if (elapsed_ms <= delay_ms) return 0.0f;
    uint32_t time_ms = elapsed_ms - delay_ms;
    if (time_ms < 90U) return premium_interval(time_ms, 0U, 90U);
    if (time_ms < 230U) return 1.0f - premium_interval(time_ms, 90U, 230U);
    return 0.0f;
}

static void premium_blend(spring_value_t *value, float neutral, float weight)
{
    value->target = neutral + (value->target - neutral) * weight;
}

static void premium_apply_eye_story(eye_pose_t *eye,
                                    uint32_t elapsed_ms,
                                    uint32_t delay_ms,
                                    uint32_t duration_ms,
                                    float strength)
{
    float weight = premium_story_weight(elapsed_ms, delay_ms, duration_ms) * strength;
    float anticipation = premium_anticipation(elapsed_ms, delay_ms) * strength;
    premium_blend(&eye->width, 32.0f, weight);
    premium_blend(&eye->height, 40.0f, weight);
    premium_blend(&eye->offset_x, 0.0f, weight);
    premium_blend(&eye->offset_y, 0.0f, weight);
    premium_blend(&eye->outer_lid, 0.0f, weight);
    premium_blend(&eye->inner_lid, 0.0f, weight);
    premium_blend(&eye->smile_cut, 0.0f, weight);
    eye->width.target += anticipation * 3.5f;
    eye->height.target -= anticipation * 5.5f;
    eye->offset_y.target += anticipation * 1.5f;
}

static void premium_apply_story(uint32_t elapsed_ms,
                                uint32_t duration_ms,
                                uint8_t intensity)
{
    float strength = (float)intensity / 100.0f;
    premium_apply_eye_story(&s_left_eye, elapsed_ms, 0U, duration_ms, strength);
    premium_apply_eye_story(&s_right_eye, elapsed_ms, 22U, duration_ms, strength);
    float weight = premium_story_weight(elapsed_ms, 72U, duration_ms) * strength;
    premium_blend(&s_mouth.width, 25.0f, weight);
    premium_blend(&s_mouth.curve, 0.0f, weight);
    premium_blend(&s_mouth.open_height, 7.0f, weight);
    premium_blend(&s_mouth.offset_x, 0.0f, weight);
    premium_blend(&s_mouth.offset_y, 0.0f, weight);
    if (s_motion.gaze_x > 0) {
        s_left_eye.width.target -= 1.0f;
        s_right_eye.width.target += 1.0f;
    } else if (s_motion.gaze_x < 0) {
        s_left_eye.width.target += 1.0f;
        s_right_eye.width.target -= 1.0f;
    }
}

static void premium_update_spring(spring_value_t *value, float delta_seconds)
{
    float step = delta_seconds / 4.0f;
    for (int index = 0; index < 4; ++index) {
        float acceleration = (value->target - value->value) * 180.0f -
                             value->velocity * 25.0f;
        value->velocity += acceleration * step;
        value->value += value->velocity * step;
    }
}

static void premium_update_pose(void)
{
    spring_value_t *values[] = {
        &s_left_eye.width, &s_left_eye.height, &s_left_eye.offset_x,
        &s_left_eye.offset_y, &s_left_eye.outer_lid, &s_left_eye.inner_lid,
        &s_left_eye.smile_cut, &s_right_eye.width, &s_right_eye.height,
        &s_right_eye.offset_x, &s_right_eye.offset_y, &s_right_eye.outer_lid,
        &s_right_eye.inner_lid, &s_right_eye.smile_cut, &s_mouth.width,
        &s_mouth.curve, &s_mouth.open_height, &s_mouth.offset_x, &s_mouth.offset_y,
    };
    size_t count = sizeof(values) / sizeof(values[0]);
    if (!s_pose_initialized) {
        for (size_t index = 0; index < count; ++index) {
            values[index]->value = values[index]->target;
            values[index]->velocity = 0.0f;
        }
        s_pose_initialized = true;
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        premium_update_spring(values[index], s_motion.delta_seconds);
    }
}

static void premium_draw_eye(const eye_pose_t *eye, bool is_left,
                             int base_x, int base_y,
                             uint16_t cyan, uint16_t black)
{
    int width = rounded_float(eye->width.value);
    int height = rounded_float(eye->height.value);
    int blink = is_left ? s_left_blink : s_right_blink;
    width += blink * 7 / 1000;
    height = height * (1000 - blink) / 1000;
    int center_x = base_x + rounded_float(eye->offset_x.value) + s_eye_offset_x;
    int center_y = base_y + rounded_float(eye->offset_y.value) + s_eye_offset_y;
    if (width < 8) width = 8;
    if (height < 4) height = 4;
    fill_round_rect_center(center_x, center_y, width, height,
                           (width < height ? width : height) / 2, cyan);

    int half_width = width / 2;
    int half_height = height / 2;
    int outer = rounded_float(eye->outer_lid.value);
    int inner = rounded_float(eye->inner_lid.value);
    int left = is_left ? outer : inner;
    int right = is_left ? inner : outer;
    if (left > 0 || right > 0) {
        int denominator = width > 1 ? width - 1 : 1;
        for (int column = 0; column < width; ++column) {
            int cover = left + (right - left) * column / denominator;
            if (cover > 0) {
                fill_rect(center_x - half_width + column,
                          center_y - half_height - 1, 1, cover + 2, black);
            }
        }
    }
    int smile = rounded_float(eye->smile_cut.value);
    if (smile > 1 && blink < 800) {
        fill_ellipse(center_x, center_y + half_height + 2,
                     half_width + 2, smile + 4, black);
    }
}

static bool premium_expression_can_speak(baby_tank_expression_t expression)
{
    return expression != BABY_TANK_EXPRESSION_SLEEPY &&
           expression != BABY_TANK_EXPRESSION_SLEEPING &&
           expression != BABY_TANK_EXPRESSION_BUBBLE &&
           expression != BABY_TANK_EXPRESSION_LISTENING &&
           expression != BABY_TANK_EXPRESSION_PROCESSING;
}

static void premium_draw_mouth(baby_tank_expression_t expression,
                               uint8_t speech_level,
                               uint16_t cream)
{
    int width = rounded_float(s_mouth.width.value);
    int curve = rounded_float(s_mouth.curve.value);
    int open = rounded_float(s_mouth.open_height.value);
    int x = 120 + rounded_float(s_mouth.offset_x.value);
    int y = 146 + rounded_float(s_mouth.offset_y.value);
    if (speech_level > 4U && premium_expression_can_speak(expression)) {
        int speech_width = width + (int)speech_level * 5 / 100;
        int speech_open = 5 + (int)speech_level * 12 / 100;
        y += curve / 4;
        fill_round_rect_center(x, y, speech_width, speech_open,
                               speech_open / 2, cream);
        return;
    }
    if (open >= 5 && curve > -2 && curve < 2) {
        fill_round_rect_center(x, y, width, open, open / 2, cream);
    } else {
        draw_curve(x - width / 2, y, x, y + curve, x + width / 2, y, 3, cream);
    }
}

static int premium_event(uint32_t elapsed_ms, uint32_t start_ms, uint32_t end_ms)
{
    if (end_ms <= start_ms + 2U) return 0;
    uint32_t ramp = (end_ms - start_ms) / 3U;
    if (ramp > 180U) ramp = 180U;
    if (elapsed_ms <= start_ms || elapsed_ms >= end_ms) return 0;
    if (elapsed_ms < start_ms + ramp) {
        return rounded_float(premium_interval(elapsed_ms, start_ms,
                                              start_ms + ramp) * 1000.0f);
    }
    if (elapsed_ms > end_ms - ramp) {
        return rounded_float((1.0f - premium_interval(elapsed_ms,
                                                      end_ms - ramp, end_ms)) * 1000.0f);
    }
    return 1000;
}

static void premium_draw_cue(baby_tank_expression_t expression,
                             uint32_t elapsed_ms,
                             uint32_t duration_ms,
                             uint8_t intensity,
                             uint16_t cyan, uint16_t cream, uint16_t black)
{
    uint32_t exit_start = premium_exit_start(duration_ms);
    uint32_t cue_end = exit_start;
    if (cue_end > 1700U) cue_end = 1700U;
    uint32_t tear_end = exit_start;
    if (tear_end > 2850U) tear_end = 2850U;
    int cue = premium_event(elapsed_ms, 520U, cue_end) * intensity / 100;
    if (expression == BABY_TANK_EXPRESSION_EXCITED && cue > 0) {
        int reach = 5 + cue * 5 / 1000;
        draw_line(42,83,42-reach,78-reach/2,2,cyan);
        draw_line(198,83,198+reach,78-reach/2,2,cyan);
        draw_line(43,132,43-reach,135+reach/2,2,cyan);
        draw_line(197,132,197+reach,135+reach/2,2,cyan);
    } else if (expression == BABY_TANK_EXPRESSION_LOVE && cue > 0) {
        draw_heart(120-s_eye_offset_x, 73-s_eye_offset_y, 7+cue*5/1000, cyan);
    } else if (expression == BABY_TANK_EXPRESSION_SHY && cue > 0) {
        int length = 3 + cue * 5 / 1000;
        draw_line(42,139,42+length,136,1,cream);
        draw_line(49,142,49+length,139,1,cream);
        draw_line(198,139,198-length,136,1,cream);
        draw_line(191,142,191-length,139,1,cream);
    } else if (expression == BABY_TANK_EXPRESSION_COMPLAINING && cue > 0) {
        int shift = cue * 4 / 1000;
        draw_line(87-shift,142,80-shift,139,2,cream);
        draw_line(86-shift,149,77-shift,149,2,cream);
        draw_line(87-shift,156,80-shift,159,2,cream);
    } else if ((expression == BABY_TANK_EXPRESSION_ANGRY ||
                expression == BABY_TANK_EXPRESSION_FURIOUS) && cue > 0) {
        int lift = cue * 5 / 1000;
        draw_line(194,78-lift,202,70-lift,2,cyan);
        draw_line(202,70-lift,211,75-lift,2,cyan);
        draw_line(202,70-lift,203,61-lift,2,cyan);
    } else if (expression == BABY_TANK_EXPRESSION_CRYING &&
               elapsed_ms > 720U && elapsed_ms < tear_end) {
        int y = 124 + (int)(elapsed_ms - 720U) * 46 /
                      (int)(tear_end - 720U);
        draw_teardrop(69,y,8,cyan);
        draw_teardrop(171,y+8,8,cyan);
    } else if (expression == BABY_TANK_EXPRESSION_WORRIED && cue > 0) {
        draw_teardrop(199,91+cue*4/1000,8,cyan);
    } else if (expression == BABY_TANK_EXPRESSION_DIZZY &&
               elapsed_ms > 420U && elapsed_ms < exit_start) {
        draw_line(59+s_eye_offset_x,100+s_eye_offset_y,
                  81+s_eye_offset_x,124+s_eye_offset_y,3,black);
        draw_line(81+s_eye_offset_x,100+s_eye_offset_y,
                  59+s_eye_offset_x,124+s_eye_offset_y,3,black);
        draw_line(159+s_eye_offset_x,100+s_eye_offset_y,
                  181+s_eye_offset_x,124+s_eye_offset_y,3,black);
        draw_line(181+s_eye_offset_x,100+s_eye_offset_y,
                  159+s_eye_offset_x,124+s_eye_offset_y,3,black);
    } else if (expression == BABY_TANK_EXPRESSION_THINKING && cue > 0) {
        draw_bubble(190,82,3+cue*2/1000,cyan);
        draw_bubble(204,67,5+cue*3/1000,cyan);
    } else if (expression == BABY_TANK_EXPRESSION_SLEEPY && cue > 0) {
        draw_bubble(194,82-cue*8/1000,4+cue*2/1000,cyan);
    } else if (expression == BABY_TANK_EXPRESSION_BUBBLE) {
        uint32_t bubble_end = exit_start;
        if (bubble_end > 3500U) bubble_end = 3500U;
        int bubble = premium_event(elapsed_ms,650U,bubble_end) * intensity / 100;
        if (bubble > 0) {
            int radius = 4 + bubble * 10 / 1000;
            draw_bubble(137+radius,145-radius,radius,cyan);
        }
    }
}

static void premium_draw_sleep_details(uint32_t elapsed_ms,
                                       uint16_t cyan,
                                       uint16_t cream,
                                       uint16_t black)
{
    /* The nose bubble owns the breathing rhythm. It remains attached instead
     * of floating away, like a compact manga snore cue. */
    int breath = smooth_wave(elapsed_ms + 420U, 3200U, 6);
    int bubble_radius = 10 + breath;
    if (bubble_radius < 4) bubble_radius = 4;
    int bubble_x = 109 - bubble_radius / 2;
    int bubble_y = 139 - breath / 4;
    draw_line(112, 139, bubble_x + bubble_radius - 1, bubble_y,
              2, cyan);
    fill_circle(bubble_x, bubble_y, bubble_radius + 1, cyan);
    fill_circle(bubble_x - 1, bubble_y, bubble_radius - 2, black);
    fill_circle(bubble_x - bubble_radius / 3,
                bubble_y - bubble_radius / 3, 2, cream);

    /* A single small drop stays on the mouth corner; it never stretches or
     * falls, and only shifts by one pixel with the slower sleep breath. */
    int drool_sway = smooth_wave(elapsed_ms + 1100U, 4300U, 1);
    draw_line(131, 155, 132 + drool_sway, 160, 2, cyan);
    fill_circle(132 + drool_sway, 162, 3, cyan);
    fill_circle(131 + drool_sway, 161, 1, cream);
}

static void premium_draw_listening_overlay(uint32_t elapsed_ms,
                                           uint16_t cyan,
                                           uint16_t cream)
{
    /* A compact manga ear sits beside the face instead of looking like a
     * system icon. Its two sound waves breathe at different rates. */
    int bob = smooth_wave(elapsed_ms, 2200U, 1);
    draw_curve(202, 91 + bob, 218, 82 + bob, 219, 101 + bob, 3, cream);
    draw_curve(219, 101 + bob, 220, 116 + bob, 208, 121 + bob, 3, cream);
    draw_curve(208, 121 + bob, 198, 116 + bob, 205, 106 + bob, 3, cream);
    draw_curve(205, 97 + bob, 214, 92 + bob, 214, 102 + bob, 2, cyan);
    draw_curve(214, 102 + bob, 214, 111 + bob, 207, 113 + bob, 2, cyan);

    int near_wave = smooth_wave(elapsed_ms + 180U, 1050U, 2);
    int far_wave = smooth_wave(elapsed_ms + 620U, 1450U, 2);
    draw_curve(224 + near_wave, 94, 231 + near_wave, 102,
               224 + near_wave, 111, 2, cyan);
    draw_curve(229 + far_wave, 88, 238 + far_wave / 2, 102,
               229 + far_wave, 117, 1, cream);
}

static void premium_draw_processing_overlay(uint32_t elapsed_ms,
                                            uint16_t cyan,
                                            uint16_t cream)
{
    static const int8_t orbit[8][2] = {
        {0, -12}, {8, -8}, {12, 0}, {8, 8},
        {0, 12}, {-8, 8}, {-12, 0}, {-8, -8},
    };
    const unsigned active = (elapsed_ms / 120U) & 7U;
    const uint16_t dim = rgb565(0, 72, 68);
    const int center_x = 202;
    const int center_y = 51;

    /* Lead-in bubbles connect the face to the loader, so it reads as a
     * comic thought rather than a generic app progress indicator. */
    fill_circle(184, 76, 2, dim);
    fill_circle(190, 67, 3, cyan);
    for (unsigned index = 0U; index < 8U; ++index) {
        unsigned trail = (active + 8U - index) & 7U;
        int radius = trail == 0U ? 4 : trail == 1U ? 3 :
                     trail == 2U ? 2 : 1;
        uint16_t color = trail == 0U ? cream :
                         trail <= 2U ? cyan : dim;
        fill_circle(center_x + orbit[index][0],
                    center_y + orbit[index][1], radius, color);
    }
}

static void premium_draw_dice(uint8_t value, uint32_t elapsed_ms,
                              uint16_t cyan, uint16_t cream,
                              uint16_t black)
{
    bool rolling = elapsed_ms < 960U;
    uint8_t shown = rolling
                        ? (uint8_t)(1U + ((elapsed_ms / 80U + value) % 6U))
                        : value;
    int shake_x = rolling ? (((elapsed_ms / 55U) & 1U) != 0U ? 6 : -6) : 0;
    int bob = rolling ? smooth_wave(elapsed_ms, 210U, 8)
                      : smooth_wave(elapsed_ms, 900U, 2);
    int cx = 120 + shake_x;
    int cy = 112 + bob;
    fill_round_rect_center(cx, cy, 112, 112, 22, cream);
    fill_round_rect_center(cx, cy, 98, 98, 18, black);
    static const int8_t positions[7][2] = {
        {0, 0}, {-11, -11}, {11, 11}, {11, -11}, {-11, 11},
        {-11, 0}, {11, 0},
    };
    bool dots[7] = {false};
    if ((shown & 1U) != 0U) dots[0] = true;
    if (shown >= 2U) dots[1] = dots[2] = true;
    if (shown >= 4U) dots[3] = dots[4] = true;
    if (shown == 6U) dots[5] = dots[6] = true;
    for (unsigned index = 0; index < 7U; ++index) {
        if (dots[index]) {
            fill_circle(cx + positions[index][0] * 3,
                        cy + positions[index][1] * 3,
                        8, index == 0U ? cream : cyan);
        }
    }
    if (rolling) {
        draw_line(39, 69, 57, 78, 3, cyan);
        draw_line(181, 150, 201, 158, 3, cream);
        draw_line(45, 159, 62, 149, 2, cream);
    }
}

static bool premium_is_full_screen_game(baby_tank_expression_t expression)
{
    return (expression >= BABY_TANK_EXPRESSION_ROCK &&
            expression <= BABY_TANK_EXPRESSION_SCISSORS) ||
           expression == BABY_TANK_EXPRESSION_FIGHTING ||
           (expression >= BABY_TANK_EXPRESSION_DICE_1 &&
            expression <= BABY_TANK_EXPRESSION_DICE_6);
}

static void premium_draw_food_item(baby_tank_expression_t expression,
                                   int x, int y,
                                   uint16_t cyan, uint16_t cream,
                                   uint16_t coral, uint16_t black)
{
    switch (expression) {
    case BABY_TANK_EXPRESSION_STRAWBERRY:
        fill_ellipse(x, y + 2, 15, 18, coral);
        draw_line(x - 10, y - 14, x, y - 21, 4, cyan);
        draw_line(x + 10, y - 14, x, y - 21, 4, cyan);
        fill_circle(x - 5, y, 2, cream);
        fill_circle(x + 5, y + 5, 2, cream);
        break;
    case BABY_TANK_EXPRESSION_APPLE:
        fill_circle(x - 7, y + 2, 15, coral);
        fill_circle(x + 7, y + 2, 15, coral);
        draw_line(x, y - 13, x + 3, y - 24, 3, cream);
        fill_ellipse(x + 11, y - 18, 8, 4, cyan);
        break;
    case BABY_TANK_EXPRESSION_BANANA:
        draw_curve(x - 20, y - 8, x - 2, y + 25,
                   x + 22, y - 8, 9, cream);
        draw_curve(x - 17, y - 10, x - 1, y + 15,
                   x + 18, y - 11, 4, black);
        break;
    case BABY_TANK_EXPRESSION_CAKE:
        fill_round_rect_center(x, y + 6, 39, 27, 5, cream);
        fill_rect(x - 19, y - 10, 39, 8, coral);
        draw_line(x, y - 11, x, y - 27, 3, cyan);
        fill_circle(x, y - 30, 4, coral);
        break;
    case BABY_TANK_EXPRESSION_COOKIE:
        fill_circle(x, y, 21, cream);
        fill_circle(x - 8, y - 7, 3, black);
        fill_circle(x + 8, y - 2, 3, black);
        fill_circle(x - 2, y + 10, 3, black);
        break;
    case BABY_TANK_EXPRESSION_BREAD:
        fill_round_rect_center(x, y + 2, 43, 31, 10, cream);
        fill_round_rect_center(x, y + 5, 34, 22, 7, coral);
        fill_round_rect_center(x, y + 6, 29, 18, 6, black);
        break;
    case BABY_TANK_EXPRESSION_RICE_BALL:
        draw_line(x, y - 24, x - 25, y + 19, 6, cream);
        draw_line(x, y - 24, x + 25, y + 19, 6, cream);
        draw_line(x - 25, y + 19, x + 25, y + 19, 6, cream);
        fill_rect(x - 9, y + 4, 18, 19, cyan);
        break;
    case BABY_TANK_EXPRESSION_ICE_CREAM:
        fill_circle(x, y - 13, 18, coral);
        fill_circle(x - 9, y - 5, 13, cream);
        draw_line(x - 15, y + 5, x, y + 30, 5, cream);
        draw_line(x + 14, y + 5, x, y + 30, 5, cream);
        break;
    case BABY_TANK_EXPRESSION_MILK:
    case BABY_TANK_EXPRESSION_JUICE:
    case BABY_TANK_EXPRESSION_WATER:
    case BABY_TANK_EXPRESSION_YOGURT:
    case BABY_TANK_EXPRESSION_SOY_MILK:
    case BABY_TANK_EXPRESSION_COCOA: {
        uint16_t body = expression == BABY_TANK_EXPRESSION_JUICE ? coral :
                        expression == BABY_TANK_EXPRESSION_WATER ? cyan : cream;
        if (expression == BABY_TANK_EXPRESSION_COCOA) {
            fill_round_rect_center(x, y + 2, 41, 32, 8, coral);
            draw_bubble(x + 25, y + 3, 10, cream);
            draw_curve(x - 7, y - 18, x - 13, y - 27,
                       x - 5, y - 33, 2, cream);
        } else {
            fill_round_rect_center(x, y + 2, 37, 46, 7, body);
            fill_round_rect_center(x, y + 2, 27, 35, 5, black);
            fill_circle(x, y + 2, 7,
                        expression == BABY_TANK_EXPRESSION_WATER ? cream : cyan);
            if (expression == BABY_TANK_EXPRESSION_YOGURT) {
                fill_rect(x - 22, y - 24, 44, 6, coral);
            }
        }
        break;
    }
    default:
        fill_circle(x, y, 14, cream);
        break;
    }
}

static void premium_draw_interaction_overlay(baby_tank_expression_t expression,
                                             uint32_t elapsed_ms,
                                             uint16_t cyan,
                                             uint16_t cream,
                                             uint16_t coral,
                                             uint16_t black)
{
    int beat = smooth_wave(elapsed_ms, 520U, 3);
    if (expression == BABY_TANK_EXPRESSION_EATING) {
        int x = 157 - (int)((elapsed_ms / 22U) % 18U);
        fill_circle(x, 151 + beat, 13, cream);
        fill_circle(x + 8, 143 + beat, 5, black);
        fill_circle(x - 5, 147 + beat, 2, cyan);
        fill_circle(x + 1, 156 + beat, 2, cyan);
        draw_line(94, 137 + beat, 101, 141 + beat, 2, cyan);
        draw_line(91, 147 + beat, 100, 148 + beat, 2, cyan);
    } else if (expression == BABY_TANK_EXPRESSION_DRINKING) {
        fill_round_rect_center(166, 158 + beat, 28, 34, 7, cyan);
        fill_round_rect_center(166, 153 + beat, 21, 21, 5, black);
        draw_line(155, 138 + beat, 145, 124, 3, cream);
        draw_line(145, 124, 131, 145, 3, cream);
        fill_circle(181, 139 + beat, 3, cream);
        fill_circle(188, 129 + beat, 2, cyan);
    } else if (expression == BABY_TANK_EXPRESSION_ROCK) {
        int enter = elapsed_ms < 220U ? (int)((220U - elapsed_ms) * 70U / 220U) : 0;
        int y = 120 + beat + enter;
        fill_round_rect_center(120, y + 22, 104, 72, 25, cyan);
        for (int finger = 0; finger < 4; ++finger) {
            fill_round_rect_center(83 + finger * 24, y - 20,
                                   25, 43, 10, cream);
        }
        fill_round_rect_center(168, y + 23, 25, 56, 10, cream);
        draw_line(48, 181, 68, 168, 4, coral);
        draw_line(185, 170, 207, 181, 4, cyan);
    } else if (expression == BABY_TANK_EXPRESSION_PAPER) {
        int enter = elapsed_ms < 220U ? (int)((220U - elapsed_ms) * 70U / 220U) : 0;
        int y = 121 + beat + enter;
        fill_round_rect_center(120, y + 27, 90, 74, 22, cyan);
        for (int finger = 0; finger < 4; ++finger) {
            fill_round_rect_center(84 + finger * 24, y - 28,
                                   20, 76 - finger * 5, 9, cream);
        }
        draw_line(161, y + 14, 195, y - 16, 17, cream);
        draw_line(44, 73, 62, 83, 3, cyan);
        draw_line(185, 66, 202, 53, 3, coral);
    } else if (expression == BABY_TANK_EXPRESSION_SCISSORS) {
        int enter = elapsed_ms < 220U ? (int)((220U - elapsed_ms) * 70U / 220U) : 0;
        int y = 125 + beat + enter;
        draw_line(105, y + 30, 75, y - 55, 22, cream);
        draw_line(128, y + 29, 165, y - 52, 22, cream);
        fill_round_rect_center(122, y + 39, 94, 66, 25, cyan);
        fill_circle(72, y - 58, 13, coral);
        fill_circle(168, y - 56, 13, cyan);
        draw_line(40, 170, 59, 160, 3, cream);
    } else if (expression == BABY_TANK_EXPRESSION_FIGHTING) {
        int shake = ((elapsed_ms / 70U) & 1U) != 0U ? 4 : -4;
        int punch = 10 + (int)((elapsed_ms / 40U) % 18U);
        fill_round_rect_center(120 + shake, 127 - punch, 112, 82, 26, cyan);
        for (int finger = 0; finger < 4; ++finger) {
            fill_round_rect_center(79 + finger * 27 + shake, 73 - punch,
                                   28, 48, 10, cream);
        }
        draw_line(36, 72, 57, 87, 4, coral);
        draw_line(35, 116, 57, 113, 4, cream);
        draw_line(185, 65, 203, 48, 4, cyan);
        draw_line(205, 100, 226, 100, 4, cream);
        draw_line(215, 89, 215, 111, 4, cream);
    } else if (expression >= BABY_TANK_EXPRESSION_DICE_1 &&
               expression <= BABY_TANK_EXPRESSION_DICE_6) {
        uint8_t value = (uint8_t)(expression - BABY_TANK_EXPRESSION_DICE_1 + 1);
        premium_draw_dice(value, elapsed_ms, cyan, cream, black);
    } else if (expression >= BABY_TANK_EXPRESSION_STRAWBERRY &&
               expression <= BABY_TANK_EXPRESSION_COCOA) {
        bool drink = expression >= BABY_TANK_EXPRESSION_MILK;
        int approach = (int)((elapsed_ms / 24U) % 24U);
        int x = drink ? 170 - approach / 3 : 170 - approach;
        int y = 151 + beat;
        premium_draw_food_item(expression, x, y, cyan, cream, coral, black);
        if (drink) {
            draw_line(151, y - 20, 139, 130, 3, cream);
        } else {
            draw_line(93, 137 + beat, 101, 141 + beat, 2, cyan);
            draw_line(90, 148 + beat, 100, 149 + beat, 2, cream);
        }
    }
}

/*
 * A short comic impact performance for the remote-control cannon.  It stays
 * predominantly black so the small LCD does not turn into a harsh flashlight:
 * anticipation -> coloured impact ring -> flying sparks -> smoke/recovery.
 */
static void premium_draw_cannon_overlay(uint32_t elapsed_ms,
                                        uint8_t intensity,
                                        uint16_t cyan,
                                        uint16_t cream,
                                        uint16_t coral,
                                        uint16_t black)
{
    static const int8_t rays[16][2] = {
        {  0,-100}, { 38,-92}, { 71,-71}, { 92,-38},
        {100,   0}, { 92, 38}, { 71, 71}, { 38, 92},
        {  0, 100}, {-38, 92}, {-71, 71}, {-92, 38},
        {-100,  0}, {-92,-38}, {-71,-71}, {-38,-92},
    };
    const int center_x = 120;
    const int center_y = 111;

    if (elapsed_ms < 190U) {
        int charge = (int)(elapsed_ms * intensity / 190U / 8U);
        int radius = 25 - charge;
        if (radius < 12) radius = 12;
        draw_line(center_x - radius - 18, center_y,
                  center_x - radius, center_y, 2, cream);
        draw_line(center_x + radius, center_y,
                  center_x + radius + 18, center_y, 2, cream);
        draw_line(center_x, center_y - radius - 14,
                  center_x, center_y - radius, 2, cyan);
        return;
    }

    uint32_t impact_ms = elapsed_ms - 190U;
    if (impact_ms < 650U) {
        int progress = (int)(impact_ms * 1000U / 650U);
        int core = 9 + progress * 28 / 1000;
        int ring = 22 + progress * 82 / 1000;
        int ray_start = 18 + progress * 40 / 1000;
        int ray_end = ray_start + 25 - progress * 8 / 1000;

        /* Two hollow rings give the impact depth without a white full-screen flash. */
        fill_circle(center_x, center_y, ring, coral);
        fill_circle(center_x, center_y, ring - 5, black);
        fill_circle(center_x, center_y, core + 7, cream);
        fill_circle(center_x, center_y, core, coral);

        for (unsigned index = 0U; index < 16U; ++index) {
            int x0 = center_x + rays[index][0] * ray_start / 100;
            int y0 = center_y + rays[index][1] * ray_start / 100;
            int x1 = center_x + rays[index][0] * ray_end / 100;
            int y1 = center_y + rays[index][1] * ray_end / 100;
            uint16_t color = (index % 3U == 0U) ? cyan :
                             (index % 3U == 1U) ? cream : coral;
            draw_line(x0, y0, x1, y1, index % 2U == 0U ? 3 : 2, color);
        }
        for (unsigned spark = 0U; spark < 7U; ++spark) {
            unsigned ray = (spark * 5U + 1U) & 15U;
            int distance = 35 + progress * (35 + (int)spark * 3) / 1000;
            fill_circle(center_x + rays[ray][0] * distance / 100,
                        center_y + rays[ray][1] * distance / 100,
                        spark % 3U == 0U ? 3 : 2,
                        spark % 2U == 0U ? cream : cyan);
        }
        return;
    }

    if (impact_ms < 1350U) {
        uint32_t smoke_ms = impact_ms - 650U;
        int rise = (int)(smoke_ms * 28U / 700U);
        int spread = 10 + (int)(smoke_ms * 20U / 700U);
        draw_bubble(center_x - spread, center_y - 8 - rise, 15, coral);
        draw_bubble(center_x + spread, center_y - 5 - rise, 18, cyan);
        draw_bubble(center_x, center_y - 22 - rise, 20, cream);
        draw_line(74, 184, 92, 176, 3, coral);
        draw_line(148, 177, 168, 185, 3, cyan);
    }
}

static void draw_expression(baby_tank_expression_t expression,
                            uint32_t elapsed_ms,
                            uint32_t duration_ms,
                            uint8_t intensity,
                            uint8_t speech_level)
{
    const uint16_t black = rgb565(0,0,0);
    const uint16_t cyan = rgb565(0,206,190);
    const uint16_t cream = rgb565(238,214,154);
    const uint16_t coral = rgb565(239,96,88);
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    fill_screen(black);
    update_life_motion(expression, now_ms);
    if (expression == BABY_TANK_EXPRESSION_CANNON &&
        elapsed_ms >= 190U && elapsed_ms < 840U) {
        int shake = 5 - (int)((elapsed_ms - 190U) * 5U / 650U);
        if (shake < 1) shake = 1;
        s_canvas_offset_x = ((elapsed_ms / 50U) & 1U) != 0U ? shake : -shake;
        s_canvas_offset_y = ((elapsed_ms / 100U) & 1U) != 0U ? 2 : -2;
    }
    premium_configure_targets(expression);
    if (expression != BABY_TANK_EXPRESSION_CALM) {
        premium_apply_story(elapsed_ms, duration_ms, intensity);
    }
    premium_update_pose();
    bool full_screen_game = premium_is_full_screen_game(expression);
    if (expression == BABY_TANK_EXPRESSION_SLEEPING) {
        int breath = smooth_wave(elapsed_ms, 2800U, 2);
        draw_curve(52, 112 + breath, 70, 119 + breath,
                   88, 112 + breath, 4, cyan);
        draw_curve(152, 113 - breath / 2, 170, 120 - breath / 2,
                   188, 113 - breath / 2, 4, cyan);
    } else if (!full_screen_game) {
        premium_draw_eye(&s_left_eye, true, 70, 112, cyan, black);
        premium_draw_eye(&s_right_eye, false, 170, 112, cyan, black);
    }
    if (!full_screen_game) {
        premium_draw_mouth(expression, speech_level, cream);
    }
    if (expression == BABY_TANK_EXPRESSION_SLEEPING) {
        premium_draw_sleep_details(elapsed_ms, cyan, cream, black);
    }
    premium_draw_cue(expression, elapsed_ms, duration_ms, intensity,
                     cyan, cream, black);
    if (expression == BABY_TANK_EXPRESSION_LISTENING) {
        premium_draw_listening_overlay(elapsed_ms, cyan, cream);
    } else if (expression == BABY_TANK_EXPRESSION_PROCESSING) {
        premium_draw_processing_overlay(elapsed_ms, cyan, cream);
    }
    premium_draw_interaction_overlay(expression, elapsed_ms,
                                     cyan, cream, coral, black);
    if (expression == BABY_TANK_EXPRESSION_CANNON) {
        premium_draw_cannon_overlay(elapsed_ms, intensity,
                                    cyan, cream, coral, black);
    }
}
