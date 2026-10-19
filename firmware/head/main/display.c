#include "display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "network_runtime.h"
#include "conversation.h"
#include "power_runtime.h"

#define LCD_WIDTH              240
#define LCD_HEIGHT             240
#define LCD_HOST               SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000)
#define LCD_PIN_MOSI           GPIO_NUM_47
#define LCD_PIN_SCLK           GPIO_NUM_21
#define LCD_PIN_CS             GPIO_NUM_44
#define LCD_PIN_DC             GPIO_NUM_43
#define LCD_PIN_RESET          GPIO_NUM_NC
#define LCD_PIN_BACKLIGHT      GPIO_NUM_46
#define LCD_BACKLIGHT_ON_LEVEL 1
#define FRAME_WAIT_MS          100

static const char *TAG = "baby_tank_display";

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_transfer_done;
static uint16_t *s_framebuffer;
static bool s_transfer_pending;
static bool s_backlight_on;
static int s_canvas_offset_x;
static int s_canvas_offset_y;
static int s_eye_offset_x;
static int s_eye_offset_y;
static int s_left_blink;
static int s_right_blink;

typedef struct {
    uint32_t random_state;
    uint32_t last_update_ms;
    uint32_t next_saccade_ms;
    uint32_t saccade_start_ms;
    uint32_t saccade_duration_ms;
    int gaze_start_x;
    int gaze_start_y;
    int gaze_target_x;
    int gaze_target_y;
    int gaze_x;
    int gaze_y;
    bool saccade_active;
    uint32_t next_blink_ms;
    uint32_t blink_start_ms;
    bool blink_active;
    bool double_blink_pending;
    float face_x;
    float face_y;
    float face_velocity_x;
    float face_velocity_y;
    float delta_seconds;
    baby_tank_expression_t previous_expression;
} life_motion_t;

static life_motion_t s_motion = {
    .previous_expression = BABY_TANK_EXPRESSION_COUNT,
};

typedef struct {
    float value;
    float velocity;
    float target;
} spring_value_t;

typedef struct {
    spring_value_t width;
    spring_value_t height;
    spring_value_t offset_x;
    spring_value_t offset_y;
    spring_value_t outer_lid;
    spring_value_t inner_lid;
    spring_value_t smile_cut;
} eye_pose_t;

typedef struct {
    spring_value_t width;
    spring_value_t curve;
    spring_value_t open_height;
    spring_value_t offset_x;
    spring_value_t offset_y;
} mouth_pose_t;

static eye_pose_t s_left_eye;
static eye_pose_t s_right_eye;
static mouth_pose_t s_mouth;
static bool s_pose_initialized;

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                      ((uint16_t)(green & 0xFCU) << 3) |
                      ((uint16_t)blue >> 3));
}

static void set_pixel(int x, int y, uint16_t color)
{
    x += s_canvas_offset_x;
    y += s_canvas_offset_y;
    if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT) {
        s_framebuffer[y * LCD_WIDTH + x] = color;
    }
}

static void fill_screen(uint16_t color)
{
    for (size_t pixel = 0; pixel < LCD_WIDTH * LCD_HEIGHT; ++pixel) {
        s_framebuffer[pixel] = color;
    }
}

static void draw_status_line(void)
{
    /* Product UI uses compact, non-verbal network affordances. A crossed
     * Wi-Fi mark means the station is disconnected; a crossed cloud means
     * Wi-Fi is associated but the Baby Tank cloud probe is unavailable. */
    bool wifi_connected = baby_tank_network_is_connected();
    baby_tank_internet_phase_t internet =
        baby_tank_network_internet_phase();
    bool cloud_unavailable = wifi_connected &&
        (internet == BABY_TANK_INTERNET_OFFLINE ||
         internet == BABY_TANK_INTERNET_PORTAL);
    bool network_issue = !wifi_connected || cloud_unavailable;
    uint8_t head_percent = 0U;
    uint8_t chassis_percent = 0U;
    bool head_low = baby_tank_power_head_percent(&head_percent) &&
                    head_percent < 20U;
    bool chassis_low = baby_tank_power_chassis_percent(&chassis_percent) &&
                       chassis_percent < 20U;
    if (!network_issue && !head_low && !chassis_low) return;

    int saved_x = s_canvas_offset_x;
    int saved_y = s_canvas_offset_y;
    s_canvas_offset_x = 0;
    s_canvas_offset_y = 0;
    const uint16_t network_icon = cloud_unavailable ? rgb565(151, 119, 66) :
                                                      rgb565(173, 76, 70);
    const uint16_t battery_icon = rgb565(183, 109, 63);
    int slot = 0;
    int cx = 218;
    const int cy = 218;
    if (network_issue && cloud_unavailable) {
        /* A tiny cloud outline, deliberately distinct from Wi-Fi loss. */
        for (int y = -8; y <= 6; ++y) {
            for (int x = -11; x <= 11; ++x) {
                bool outer = ((x + 6) * (x + 6) + (y + 1) * (y + 1) <= 36) ||
                             ((x - 1) * (x - 1) + (y + 3) * (y + 3) <= 49) ||
                             ((x - 7) * (x - 7) + (y + 1) * (y + 1) <= 25) ||
                             (y >= 0 && y <= 5 && x >= -8 && x <= 8);
                bool inner = ((x + 6) * (x + 6) + (y + 1) * (y + 1) <= 16) ||
                             ((x - 1) * (x - 1) + (y + 3) * (y + 3) <= 25) ||
                             ((x - 7) * (x - 7) + (y + 1) * (y + 1) <= 9) ||
                             (y >= 1 && y <= 3 && x >= -6 && x <= 6);
                if (outer && !inner) set_pixel(cx + x, cy + y, network_icon);
            }
        }
    } else if (network_issue) {
        /* Three restrained Wi-Fi arcs plus a dot. */
        for (int y = -9; y <= 5; ++y) {
            for (int x = -11; x <= 11; ++x) {
                int distance = x * x + y * y;
                bool arc = y <= 2 && ((distance >= 88 && distance <= 121) ||
                                      (distance >= 40 && distance <= 64) ||
                                      (distance >= 13 && distance <= 27));
                if (arc) set_pixel(cx + x, cy + y, network_icon);
            }
        }
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                set_pixel(cx + x, cy + 5 + y, network_icon);
            }
        }
    }
    if (network_issue) {
        /* The diagonal cut is common to both icons and reads at a glance. */
        for (int step = -10; step <= 10; ++step) {
            set_pixel(cx + step, cy + step / 2, network_icon);
            set_pixel(cx + step, cy + step / 2 + 1, network_icon);
        }
        ++slot;
    }

    bool low_batteries[] = {head_low, chassis_low};
    for (unsigned kind = 0U; kind < 2U; ++kind) {
        if (!low_batteries[kind]) continue;
        cx = 218 - slot * 28;
        /* Shared battery shell and near-empty fill. */
        for (int x = -9; x <= 7; ++x) {
            set_pixel(cx + x, cy - 5, battery_icon);
            set_pixel(cx + x, cy + 5, battery_icon);
        }
        for (int y = -5; y <= 5; ++y) {
            set_pixel(cx - 9, cy + y, battery_icon);
            set_pixel(cx + 7, cy + y, battery_icon);
        }
        for (int y = -2; y <= 2; ++y) {
            set_pixel(cx + 9, cy + y, battery_icon);
        }
        for (int y = -3; y <= 3; ++y) {
            set_pixel(cx - 6, cy + y, battery_icon);
            set_pixel(cx - 5, cy + y, battery_icon);
        }
        if (kind == 0U) {
            /* Two eyes identify the detachable head battery. */
            set_pixel(cx - 1, cy - 1, battery_icon);
            set_pixel(cx + 3, cy - 1, battery_icon);
            set_pixel(cx, cy + 2, battery_icon);
            set_pixel(cx + 1, cy + 2, battery_icon);
        } else {
            /* Twin tracks identify the chassis battery. */
            for (int x = -2; x <= 4; ++x) {
                set_pixel(cx + x, cy - 2, battery_icon);
                set_pixel(cx + x, cy + 2, battery_icon);
            }
        }
        ++slot;
    }
    s_canvas_offset_x = saved_x;
    s_canvas_offset_y = saved_y;
    return;
#if 0
    /* 14x16 glyphs in flash: 聆 听 中 识 别 等 待 播 放 已 断 网 出 错 了. */
    static const uint16_t glyphs[][16] = {
        {0x0000,0x0020,0x1f30,0x0a58,0x0e4c,0x0a96,0x0b24,0x0efc,0x0a08,0x0a50,0x1e30,0x0230,0x0218,0x0000,0x0000,0x0000},
        {0x0000,0x0008,0x1efc,0x1280,0x1280,0x12fe,0x1288,0x1288,0x1288,0x1e88,0x1288,0x1108,0x0108,0x0208,0x0000,0x0000},
        {0x0000,0x0080,0x0080,0x0ffc,0x0884,0x0884,0x0884,0x0884,0x0ffc,0x0884,0x0080,0x0080,0x0080,0x0000,0x0000,0x0000},
        {0x0000,0x0800,0x0cfc,0x0684,0x0084,0x1c84,0x0484,0x04fc,0x0400,0x0608,0x076c,0x06c6,0x0c82,0x0900,0x0000,0x0000},
        {0x0000,0x0002,0x0f82,0x0892,0x0892,0x0f92,0x0212,0x0212,0x1fd2,0x0252,0x0452,0x0dc2,0x108e,0x0000,0x0000,0x0000},
        {0x0000,0x0210,0x07be,0x0d48,0x1840,0x07fc,0x0040,0x0ffe,0x0010,0x0ffc,0x0210,0x0110,0x0030,0x0000,0x0000,0x0000},
        {0x0000,0x0220,0x0720,0x05fc,0x1a20,0x03fe,0x0408,0x0c08,0x1dfe,0x1488,0x04c8,0x0448,0x0408,0x0438,0x0000,0x0000},
        {0x0000,0x080c,0x09f0,0x08ac,0x1dfe,0x0870,0x0eac,0x1906,0x1bfe,0x0d24,0x09fc,0x0924,0x19fc,0x0004,0x0000,0x0000},
        {0x0000,0x0230,0x0620,0x1fbe,0x0464,0x0444,0x07a4,0x04a4,0x05a8,0x0d98,0x0918,0x1f3f,0x12c2,0x0000,0x0000,0x0000},
        {0x0000,0x0808,0x0ff8,0x0008,0x0008,0x0808,0x0ff8,0x0808,0x0800,0x0808,0x0804,0x080c,0x0ff8,0x0000,0x0000,0x0000},
        {0x0000,0x020c,0x16f2,0x16a0,0x12a0,0x1fbe,0x1228,0x12a8,0x17a8,0x1628,0x1a28,0x1048,0x1fc8,0x0088,0x0000,0x0000},
        {0x0000,0x0ffc,0x0804,0x0c9c,0x0ad4,0x0b34,0x0924,0x0b34,0x0ad4,0x0cd4,0x0804,0x081c,0x0800,0x0000,0x0000,0x0000},
        {0x0000,0x0080,0x0c8c,0x0484,0x0484,0x0484,0x07fc,0x0080,0x0884,0x0884,0x0884,0x0ffc,0x0804,0x0000,0x0000,0x0000},
        {0x0000,0x0428,0x0e28,0x0f7e,0x1028,0x1e28,0x04fe,0x077c,0x1c44,0x057c,0x0544,0x0644,0x0c7c,0x0000,0x0000,0x0000},
        {0x0000,0x0ffe,0x000c,0x0030,0x0060,0x0040,0x0040,0x0040,0x0040,0x0040,0x0040,0x0040,0x01c0,0x0000,0x0000,0x0000},
    };
    static const uint8_t listening[] = {0, 1, 2};
    static const uint8_t recognizing[] = {3, 4, 2};
    static const uint8_t waiting[] = {5, 6, 2};
    static const uint8_t speaking[] = {7, 8, 2};
    static const uint8_t offline[] = {9, 10, 11};
    static const uint8_t error[] = {12, 13, 14};
    const uint8_t *line = NULL;
    size_t count = 0;
    bool warning = false;
    baby_tank_conversation_snapshot_t snapshot = {0};
    baby_tank_conversation_get_snapshot(&snapshot);
    if (baby_tank_network_internet_phase() != BABY_TANK_INTERNET_ONLINE ||
        snapshot.phase == BABY_TANK_CONVERSATION_DISCONNECTED) {
        line = offline; count = sizeof(offline); warning = true;
    } else {
        switch (snapshot.phase) {
        case BABY_TANK_CONVERSATION_LISTENING:
        case BABY_TANK_CONVERSATION_USER_SPEAKING:
            line = listening; count = sizeof(listening); break;
        case BABY_TANK_CONVERSATION_RECOGNIZING:
            line = recognizing; count = sizeof(recognizing); break;
        case BABY_TANK_CONVERSATION_THINKING:
            line = waiting; count = sizeof(waiting); break;
        case BABY_TANK_CONVERSATION_AI_SPEAKING:
            line = speaking; count = sizeof(speaking); break;
        case BABY_TANK_CONVERSATION_ERROR:
            line = error; count = sizeof(error); warning = true; break;
        default: break;
        }
    }
    if (line == NULL) return;
    const uint16_t background = rgb565(20, 27, 37);
    const uint16_t foreground = warning ? rgb565(245, 143, 128) :
                                          rgb565(223, 237, 248);
    const int left = (LCD_WIDTH - ((int)count * 16 + 4)) / 2;
    const int top = 216;
    for (int y = 0; y < 19; ++y) {
        for (int x = 0; x < (int)count * 16 + 4; ++x) {
            s_framebuffer[(top + y) * LCD_WIDTH + left + x] = background;
        }
    }
    for (size_t glyph = 0; glyph < count; ++glyph) {
        for (int row = 0; row < 16; ++row) {
            for (int column = 0; column < 14; ++column) {
                if (glyphs[line[glyph]][row] & (1U << (13 - column))) {
                    int x = left + 2 + (int)glyph * 16 + column;
                    int y = top + 1 + row;
                    s_framebuffer[y * LCD_WIDTH + x] = foreground;
                }
            }
        }
    }
#endif
}

static void fill_rect(int x, int y, int width, int height, uint16_t color)
{
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            set_pixel(x + column, y + row, color);
        }
    }
}

static void fill_circle(int center_x, int center_y, int radius, uint16_t color)
{
    int radius_squared = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius_squared) {
                set_pixel(center_x + x, center_y + y, color);
            }
        }
    }
}

static void fill_ellipse(int center_x, int center_y, int radius_x, int radius_y, uint16_t color)
{
    int64_t radius_x_squared = (int64_t)radius_x * radius_x;
    int64_t radius_y_squared = (int64_t)radius_y * radius_y;
    int64_t limit = radius_x_squared * radius_y_squared;

    for (int y = -radius_y; y <= radius_y; ++y) {
        for (int x = -radius_x; x <= radius_x; ++x) {
            if ((int64_t)x * x * radius_y_squared +
                    (int64_t)y * y * radius_x_squared <= limit) {
                set_pixel(center_x + x, center_y + y, color);
            }
        }
    }
}

static void fill_round_rect_center(int center_x, int center_y,
                                   int width, int height, int radius,
                                   uint16_t color)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    int half_width = width / 2;
    int half_height = height / 2;
    int maximum_radius = half_width < half_height ? half_width : half_height;
    if (radius > maximum_radius) {
        radius = maximum_radius;
    }
    if (radius < 1) {
        radius = 1;
    }

    fill_rect(center_x - half_width + radius,
              center_y - half_height,
              width - radius * 2,
              height,
              color);
    fill_rect(center_x - half_width,
              center_y - half_height + radius,
              width,
              height - radius * 2,
              color);
    fill_circle(center_x - half_width + radius,
                center_y - half_height + radius,
                radius,
                color);
    fill_circle(center_x + half_width - radius,
                center_y - half_height + radius,
                radius,
                color);
    fill_circle(center_x - half_width + radius,
                center_y + half_height - radius,
                radius,
                color);
    fill_circle(center_x + half_width - radius,
                center_y + half_height - radius,
                radius,
                color);
}

static void draw_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    int delta_x = abs(x1 - x0);
    int step_x = x0 < x1 ? 1 : -1;
    int delta_y = -abs(y1 - y0);
    int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x + delta_y;

    while (true) {
        fill_circle(x0, y0, thickness, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int twice_error = 2 * error;
        if (twice_error >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (twice_error <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

static void draw_curve(int x0, int y0,
                       int control_x, int control_y,
                       int x1, int y1,
                       int thickness,
                       uint16_t color)
{
    const int steps = 24;
    int previous_x = x0;
    int previous_y = y0;

    for (int step = 1; step <= steps; ++step) {
        int inverse = steps - step;
        int denominator = steps * steps;
        int x = (inverse * inverse * x0 + 2 * inverse * step * control_x +
                 step * step * x1) / denominator;
        int y = (inverse * inverse * y0 + 2 * inverse * step * control_y +
                 step * step * y1) / denominator;
        draw_line(previous_x, previous_y, x, y, thickness, color);
        previous_x = x;
        previous_y = y;
    }
}

static int blink_for_eye(int center_x)
{
    return center_x < LCD_WIDTH / 2 ? s_left_blink : s_right_blink;
}

static void draw_outline_eye(int center_x, int center_y,
                             int radius_x, int radius_y,
                             uint16_t color)
{
    center_x += s_eye_offset_x;
    center_y += s_eye_offset_y;
    int openness = 1000 - blink_for_eye(center_x - s_eye_offset_x);
    int visible_radius_y = radius_y * openness / 1000;

    if (visible_radius_y <= 3) {
        draw_line(center_x - radius_x, center_y,
                  center_x + radius_x, center_y, 4, color);
        return;
    }

    fill_ellipse(center_x, center_y, radius_x, visible_radius_y, color);
    int inner_radius_x = radius_x - 5;
    int inner_radius_y = visible_radius_y - 5;
    if (inner_radius_x > 0 && inner_radius_y > 0) {
        fill_ellipse(center_x, center_y, inner_radius_x, inner_radius_y, rgb565(0, 0, 0));
    }
}

static void draw_closed_eye(int center_x, int center_y, int width, int lift, uint16_t color)
{
    center_x += s_eye_offset_x;
    center_y += s_eye_offset_y;
    draw_curve(center_x - width / 2, center_y,
               center_x, center_y - lift,
               center_x + width / 2, center_y,
               5, color);
}

static void draw_eye_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    draw_line(x0 + s_eye_offset_x, y0 + s_eye_offset_y,
              x1 + s_eye_offset_x, y1 + s_eye_offset_y,
              thickness, color);
}

static void draw_smile(int center_x, int center_y, int width, int depth, uint16_t color)
{
    draw_curve(center_x - width / 2, center_y,
               center_x, center_y + depth,
               center_x + width / 2, center_y,
               4, color);
}

static void draw_heart(int center_x, int center_y, int size, uint16_t color)
{
    center_x += s_eye_offset_x;
    center_y += s_eye_offset_y;
    int radius = size / 3;
    fill_circle(center_x - radius, center_y - radius / 2, radius, color);
    fill_circle(center_x + radius, center_y - radius / 2, radius, color);
    for (int y = 0; y <= size; ++y) {
        int half_width = size - y;
        fill_rect(center_x - half_width / 2, center_y + y / 2, half_width, 1, color);
    }
}

static void draw_x_eye(int center_x, int center_y, int size, int thickness, uint16_t color)
{
    center_x += s_eye_offset_x;
    center_y += s_eye_offset_y;
    draw_line(center_x - size, center_y - size, center_x + size, center_y + size, thickness, color);
    draw_line(center_x - size, center_y + size, center_x + size, center_y - size, thickness, color);
}

static void draw_wavy_mouth(int center_x, int center_y, int width, int amplitude, uint16_t color)
{
    draw_curve(center_x - width / 2, center_y,
               center_x - width / 4, center_y - amplitude,
               center_x, center_y,
               3, color);
    draw_curve(center_x, center_y,
               center_x + width / 4, center_y + amplitude,
               center_x + width / 2, center_y,
               3, color);
}

static void draw_teardrop(int x, int y, int size, uint16_t color)
{
    fill_circle(x, y + size / 2, size / 2, color);
    for (int row = 0; row < size; ++row) {
        int half_width = row / 2;
        fill_rect(x - half_width, y - size / 2 + row, half_width * 2 + 1, 1, color);
    }
}

static void draw_bubble(int x, int y, int radius, uint16_t color)
{
    fill_circle(x, y, radius, color);
    if (radius > 3) {
        fill_circle(x, y, radius - 3, rgb565(0, 0, 0));
    }
}

static uint32_t next_random(void)
{
    uint32_t value = s_motion.random_state;
    if (value == 0) {
        value = 0x7A4D2B19U;
    }
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    s_motion.random_state = value;
    return value;
}

static uint32_t random_between(uint32_t minimum, uint32_t maximum)
{
    return minimum + next_random() % (maximum - minimum + 1U);
}

static bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return (int32_t)(now_ms - target_ms) >= 0;
}

static float smoothstep(float value)
{
    if (value <= 0.0f) {
        return 0.0f;
    }
    if (value >= 1.0f) {
        return 1.0f;
    }
    return value * value * (3.0f - 2.0f * value);
}

static int smooth_wave(uint32_t time_ms, uint32_t period_ms, int amplitude)
{
    uint32_t phase = time_ms % period_ms;
    float unit = (float)phase / (float)period_ms;
    float triangle = unit < 0.5f ? unit * 2.0f : (1.0f - unit) * 2.0f;
    float eased = smoothstep(triangle);
    return (int)((eased * 2.0f - 1.0f) * (float)amplitude);
}

static int pulse(uint32_t time_ms, uint32_t period_ms, int amplitude)
{
    return smooth_wave(time_ms, period_ms, amplitude) + amplitude;
}

static int blink_amount(uint32_t now_ms, uint32_t eye_delay_ms)
{
    uint32_t eye_start = s_motion.blink_start_ms + eye_delay_ms;
    if (!s_motion.blink_active || !time_reached(now_ms, eye_start)) {
        return 0;
    }

    uint32_t phase = now_ms - eye_start;
    if (phase < 75U) {
        return (int)(phase * 1000U / 75U);
    }
    if (phase < 105U) {
        return 1000;
    }
    if (phase < 265U) {
        return 1000 - (int)((phase - 105U) * 1000U / 160U);
    }
    return 0;
}

static int rounded_float(float value)
{
    return (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static bool expression_is_sleeping(baby_tank_expression_t expression)
{
    return expression == BABY_TANK_EXPRESSION_SLEEPING ||
           expression == BABY_TANK_EXPRESSION_SLEEPY ||
           expression == BABY_TANK_EXPRESSION_BUBBLE;
}

static bool expression_uses_gaze(baby_tank_expression_t expression)
{
    switch (expression) {
    case BABY_TANK_EXPRESSION_CALM:
    case BABY_TANK_EXPRESSION_EXCITED:
    case BABY_TANK_EXPRESSION_CURIOUS:
    case BABY_TANK_EXPRESSION_SURPRISED:
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
        if (s_motion.previous_expression != BABY_TANK_EXPRESSION_COUNT) {
            s_motion.face_y = 3.0f;
            s_motion.face_velocity_y = -55.0f;
            s_motion.face_x = (float)((int)random_between(0U, 4U) - 2);
            s_motion.face_velocity_x = -s_motion.face_x * 12.0f;
            s_motion.blink_start_ms = now_ms;
            s_motion.blink_active = true;
            s_motion.double_blink_pending = false;
        }
        s_motion.previous_expression = expression;
    }

    float force_x = -s_motion.face_x * 58.0f;
    float force_y = -s_motion.face_y * 72.0f;
    s_motion.face_velocity_x += (force_x - s_motion.face_velocity_x * 12.0f) * delta_seconds;
    s_motion.face_velocity_y += (force_y - s_motion.face_velocity_y * 11.0f) * delta_seconds;
    s_motion.face_x += s_motion.face_velocity_x * delta_seconds;
    s_motion.face_y += s_motion.face_velocity_y * delta_seconds;

    if (!expression_uses_gaze(expression)) {
        s_motion.gaze_target_x = 0;
        s_motion.gaze_target_y = 0;
        s_motion.gaze_x = s_motion.gaze_x * 3 / 4;
        s_motion.gaze_y = s_motion.gaze_y * 3 / 4;
        s_motion.saccade_active = false;
        s_motion.next_saccade_ms = now_ms + random_between(900U, 1800U);
    } else {
        if (s_motion.saccade_active) {
            uint32_t elapsed = now_ms - s_motion.saccade_start_ms;
            if (elapsed >= s_motion.saccade_duration_ms) {
                s_motion.gaze_x = s_motion.gaze_target_x;
                s_motion.gaze_y = s_motion.gaze_target_y;
                s_motion.saccade_active = false;
                s_motion.next_saccade_ms = now_ms + random_between(350U, 2400U);
            } else {
                float amount = smoothstep((float)elapsed / (float)s_motion.saccade_duration_ms);
                s_motion.gaze_x = s_motion.gaze_start_x +
                    rounded_float((float)(s_motion.gaze_target_x - s_motion.gaze_start_x) * amount);
                s_motion.gaze_y = s_motion.gaze_start_y +
                    rounded_float((float)(s_motion.gaze_target_y - s_motion.gaze_start_y) * amount);
            }
        } else if (time_reached(now_ms, s_motion.next_saccade_ms)) {
            s_motion.gaze_start_x = s_motion.gaze_x;
            s_motion.gaze_start_y = s_motion.gaze_y;
            s_motion.gaze_target_x = (int)random_between(0U, 12U) - 6;
            s_motion.gaze_target_y = (int)random_between(0U, 8U) - 4;
            if ((next_random() & 3U) == 0U) {
                s_motion.gaze_target_x = 0;
                s_motion.gaze_target_y = 0;
            }
            s_motion.saccade_duration_ms = random_between(75U, 145U);
            s_motion.saccade_start_ms = now_ms;
            s_motion.saccade_active = true;
        }
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
               time_reached(now_ms, s_motion.next_blink_ms)) {
        s_motion.blink_start_ms = now_ms;
        s_motion.blink_active = true;
        s_motion.double_blink_pending = (next_random() % 7U) == 0U;
    }

    s_canvas_offset_x = rounded_float(s_motion.face_x);
    s_canvas_offset_y = rounded_float(s_motion.face_y) + smooth_wave(now_ms, 3100U, 1);
    s_eye_offset_x = s_motion.gaze_x;
    s_eye_offset_y = s_motion.gaze_y;
    s_left_blink = blink_amount(now_ms, 0U);
    s_right_blink = blink_amount(now_ms, 14U);
}

static void draw_expression(baby_tank_expression_t expression, uint32_t elapsed_ms)
{
    const uint16_t black = rgb565(0, 0, 0);
    const uint16_t cyan = rgb565(0, 168, 154);
    const uint16_t amber = rgb565(184, 146, 74);
    const uint16_t red = rgb565(190, 55, 48);
    const uint16_t blue = rgb565(50, 105, 170);
    const uint16_t green = rgb565(75, 140, 82);
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int bob = smooth_wave(elapsed_ms, 1800U, 2);
    int slow = smooth_wave(elapsed_ms, 3200U, 4);
    int fast = smooth_wave(elapsed_ms, 180U, 3);
    baby_tank_conversation_snapshot_t conversation = {0};
    bool has_conversation =
        baby_tank_conversation_get_snapshot(&conversation) == ESP_OK;
    bool is_listening = has_conversation &&
        (conversation.phase == BABY_TANK_CONVERSATION_LISTENING ||
         conversation.phase == BABY_TANK_CONVERSATION_USER_SPEAKING);
    bool is_thinking = has_conversation &&
        (conversation.phase == BABY_TANK_CONVERSATION_RECOGNIZING ||
         conversation.phase == BABY_TANK_CONVERSATION_THINKING);

    fill_screen(black);
    update_life_motion(expression, now_ms);

    switch (expression) {
    case BABY_TANK_EXPRESSION_CALM:
        draw_outline_eye(76, 106 + bob, 22, 25, cyan);
        draw_outline_eye(164, 106 + bob, 22, 25, cyan);
        draw_line(108, 158, 132, 158, 4, amber);
        break;
    case BABY_TANK_EXPRESSION_HAPPY:
        draw_closed_eye(76, 111 + bob, 54, 22, cyan);
        draw_closed_eye(164, 111 + bob, 54, 22, cyan);
        draw_smile(120, 153 + bob, 36, 19, amber);
        break;
    case BABY_TANK_EXPRESSION_LAUGHING:
        draw_closed_eye(76, 105 + bob, 56, 24, cyan);
        draw_closed_eye(164, 105 + bob, 56, 24, cyan);
        fill_ellipse(120, 157 + bob, 18, 12 + pulse(elapsed_ms, 700, 4), amber);
        break;
    case BABY_TANK_EXPRESSION_EXCITED: {
        int grow = pulse(elapsed_ms, 650, 3);
        draw_outline_eye(76, 104 + bob, 25 + grow, 27 + grow, cyan);
        draw_outline_eye(164, 104 + bob, 25 + grow, 27 + grow, cyan);
        draw_line(34, 82, 44, 77 - grow, 2, cyan);
        draw_line(196, 77 - grow, 206, 82, 2, cyan);
        draw_line(37, 126, 46, 130 + grow, 2, cyan);
        draw_line(194, 130 + grow, 203, 126, 2, cyan);
        fill_ellipse(120, 160 + bob, 13 + grow / 2, 10 + grow, amber);
        break;
    }
    case BABY_TANK_EXPRESSION_LOVE: {
        int grow = 18 + pulse(elapsed_ms, 900, 3);
        draw_heart(76, 98 + bob, grow, cyan);
        draw_heart(164, 98 + bob, grow, cyan);
        draw_smile(120, 157, 34, 17, amber);
        break;
    }
    case BABY_TANK_EXPRESSION_CURIOUS:
        if (is_listening) {
            int listen = smooth_wave(elapsed_ms, 1300U, 3);
            draw_outline_eye(76, 103 - listen / 2, 27 + listen / 3,
                             30 + listen / 2, cyan);
            draw_outline_eye(164, 103 + listen / 2, 27 - listen / 3,
                             30 - listen / 2, cyan);
            draw_bubble(120, 159, 7 + pulse(elapsed_ms, 1100U, 1), amber);
            int tick = pulse(elapsed_ms, 900U, 2);
            draw_line(34 - tick, 91, 27 - tick, 87, 2, cyan);
            draw_line(206 + tick, 87, 213 + tick, 91, 2, cyan);
            draw_line(31 - tick, 105, 23 - tick, 105, 2, cyan);
            draw_line(209 + tick, 105, 217 + tick, 105, 2, cyan);
        } else {
            draw_outline_eye(76, 104, 24 + slow / 2, 27 + slow / 2, cyan);
            draw_outline_eye(164, 104, 21 - slow / 3, 24 - slow / 3, cyan);
            draw_bubble(120, 159, 8, amber);
        }
        break;
    case BABY_TANK_EXPRESSION_SURPRISED: {
        int grow = pulse(elapsed_ms, 1000, 4);
        draw_outline_eye(76, 103, 24 + grow, 27 + grow, cyan);
        draw_outline_eye(164, 103, 24 + grow, 27 + grow, cyan);
        draw_bubble(120, 161, 8 + grow / 2, amber);
        break;
    }
    case BABY_TANK_EXPRESSION_SHY:
        draw_closed_eye(78, 112 + bob, 48, 10, cyan);
        draw_closed_eye(162, 112 + bob, 48, 10, cyan);
        draw_teardrop(199, 91 + pulse(elapsed_ms, 1700U, 2), 9, blue);
        draw_smile(120 + slow, 158, 24, 10, amber);
        break;
    case BABY_TANK_EXPRESSION_PROUD:
        draw_eye_line(52, 102, 98, 111, 6, cyan);
        draw_eye_line(142, 111, 188, 102, 6, cyan);
        draw_curve(105, 158, 125, 168 + slow / 2, 143, 153, 4, amber);
        break;
    case BABY_TANK_EXPRESSION_BORED:
        draw_outline_eye(76, 112, 23, 13, cyan);
        draw_outline_eye(164, 112, 23, 13, cyan);
        fill_rect(50, 96, 52, 13, black);
        fill_rect(138, 96, 52, 13, black);
        draw_line(105, 160 + bob, 135, 160 + bob, 4, amber);
        break;
    case BABY_TANK_EXPRESSION_ANNOYED:
        draw_eye_line(51, 96 + fast, 99, 111 + fast, 6, cyan);
        draw_eye_line(141, 111 + fast, 189, 96 + fast, 6, cyan);
        draw_line(104, 160 + fast, 136, 160 + fast, 4, amber);
        break;
    case BABY_TANK_EXPRESSION_COMPLAINING:
        draw_closed_eye(76, 108, 50, 4 + slow, cyan);
        draw_outline_eye(164, 106, 23, 19, cyan);
        draw_line(83, 150 + fast, 91, 147 + fast, 2, amber);
        draw_line(82, 160 + fast, 91, 160 + fast, 2, amber);
        draw_line(83, 170 + fast, 91, 173 + fast, 2, amber);
        fill_ellipse(120 + slow, 160, 20 + pulse(elapsed_ms, 500, 4), 8, amber);
        break;
    case BABY_TANK_EXPRESSION_ANGRY:
        draw_eye_line(51, 94, 99, 113, 7, red);
        draw_eye_line(141, 113, 189, 94, 7, red);
        draw_line(194, 76, 202, 68, 3, red);
        draw_line(202, 68, 211, 73, 3, red);
        draw_line(202, 68, 203, 58, 3, red);
        draw_smile(120, 169, 40, -18, red);
        break;
    case BABY_TANK_EXPRESSION_FURIOUS:
        draw_eye_line(49 + fast, 91, 101 + fast, 115, 8, red);
        draw_eye_line(139 + fast, 115, 191 + fast, 91, 8, red);
        draw_line(28, 82 + fast, 41, 76 + fast, 3, red);
        draw_line(199, 76 - fast, 212, 82 - fast, 3, red);
        draw_wavy_mouth(120 + fast, 163, 46, 10 + pulse(elapsed_ms, 350, 3), red);
        break;
    case BABY_TANK_EXPRESSION_SAD:
        draw_closed_eye(76, 110 + bob, 50, -17, blue);
        draw_closed_eye(164, 110 + bob, 50, -17, blue);
        draw_smile(120, 169, 38, -17, amber);
        break;
    case BABY_TANK_EXPRESSION_CRYING: {
        int tear_y = 122 + (int)((elapsed_ms / 18U) % 48U);
        draw_closed_eye(76, 107, 50, -16, blue);
        draw_closed_eye(164, 107, 50, -16, blue);
        draw_teardrop(68, tear_y, 10, blue);
        draw_teardrop(172, 122 + ((tear_y + 24) % 48), 10, blue);
        draw_smile(120, 169, 40, -18, amber);
        break;
    }
    case BABY_TANK_EXPRESSION_WORRIED:
        draw_eye_line(51, 111, 99, 94 + bob, 6, blue);
        draw_eye_line(141, 94 + bob, 189, 111, 6, blue);
        draw_teardrop(200, 92 + pulse(elapsed_ms, 1900U, 3), 10, blue);
        draw_wavy_mouth(120, 163, 40, 8 + pulse(elapsed_ms, 900, 3), amber);
        break;
    case BABY_TANK_EXPRESSION_CONFUSED:
        draw_outline_eye(76, 105, 27 + slow / 2, 27 + slow / 2, cyan);
        draw_closed_eye(164, 110, 48, 3, cyan);
        draw_line(106, 166 + slow, 136, 154 + slow, 4, amber);
        break;
    case BABY_TANK_EXPRESSION_DIZZY:
        draw_x_eye(76 + slow, 106, 18, 5, cyan);
        draw_x_eye(164 + slow, 106, 18, 5, cyan);
        draw_bubble(120 + fast, 163, 10, amber);
        break;
    case BABY_TANK_EXPRESSION_SICK:
        draw_closed_eye(76, 111 + bob, 48, -8, green);
        draw_closed_eye(164, 111 + bob, 48, -8, green);
        draw_wavy_mouth(120, 164 + bob, 42, 7, green);
        break;
    case BABY_TANK_EXPRESSION_THINKING: {
        draw_outline_eye(76, 105 + slow / 4, 23, 25, cyan);
        draw_outline_eye(164, 105 - slow / 4, 23, 25, cyan);
        draw_curve(111, 160, 128 + slow, 166, 141, 157, 4, amber);
        if (is_thinking) {
            unsigned active_dot = (elapsed_ms / 320U) % 3U;
            for (unsigned dot = 0; dot < 3U; ++dot) {
                int radius = dot == active_dot ? 4 : 2;
                fill_circle(188 + (int)dot * 12, 69 - (int)dot * 3,
                            radius, dot == active_dot ? cyan : blue);
            }
        } else {
            draw_bubble(190, 74 + slow, 4, blue);
            draw_bubble(204, 60 + slow, 7, blue);
        }
        break;
    }
    case BABY_TANK_EXPRESSION_SLEEPY: {
        int openness = 5 + pulse(elapsed_ms, 2600, 7);
        draw_outline_eye(76, 111, 24, openness, cyan);
        draw_outline_eye(164, 111, 24, openness, cyan);
        draw_bubble(120, 164, 7 + pulse(elapsed_ms, 1400, 2), amber);
        break;
    }
    case BABY_TANK_EXPRESSION_SLEEPING:
        draw_closed_eye(76, 111 + bob, 50, 7, blue);
        draw_closed_eye(164, 111 + bob, 50, 7, blue);
        draw_bubble(120, 165 + bob, 6 + pulse(elapsed_ms, 1800, 2), amber);
        break;
    case BABY_TANK_EXPRESSION_BUBBLE: {
        int bubble_radius = 5 + pulse(elapsed_ms, 1700, 10);
        draw_closed_eye(76, 111 + bob, 50, 8, blue);
        draw_closed_eye(164, 111 + bob, 50, 8, blue);
        draw_bubble(120, 162 + bob, 5, amber);
        draw_bubble(139 + bubble_radius, 153 - bubble_radius, bubble_radius, blue);
        break;
    }
    case BABY_TANK_EXPRESSION_CANNON:
        draw_closed_eye(76, 111 + fast, 54, 15, cyan);
        draw_closed_eye(164, 111 + fast, 54, 15, cyan);
        fill_ellipse(120, 160, 12, 9, amber);
        break;
    case BABY_TANK_EXPRESSION_LISTENING:
        draw_outline_eye(76, 103, 26, 30, cyan);
        draw_outline_eye(164, 103, 26, 30, cyan);
        draw_bubble(120, 159, 7, amber);
        break;
    case BABY_TANK_EXPRESSION_PROCESSING:
        draw_outline_eye(76, 105, 23, 25, cyan);
        draw_outline_eye(164, 105, 23, 25, cyan);
        draw_curve(111, 160, 128 + slow, 166, 141, 157, 4, amber);
        break;
    case BABY_TANK_EXPRESSION_EATING:
    case BABY_TANK_EXPRESSION_DRINKING:
    case BABY_TANK_EXPRESSION_ROCK:
    case BABY_TANK_EXPRESSION_PAPER:
    case BABY_TANK_EXPRESSION_SCISSORS:
    case BABY_TANK_EXPRESSION_FIGHTING:
    case BABY_TANK_EXPRESSION_DICE_1:
    case BABY_TANK_EXPRESSION_DICE_2:
    case BABY_TANK_EXPRESSION_DICE_3:
    case BABY_TANK_EXPRESSION_DICE_4:
    case BABY_TANK_EXPRESSION_DICE_5:
    case BABY_TANK_EXPRESSION_DICE_6:
    case BABY_TANK_EXPRESSION_STRAWBERRY:
    case BABY_TANK_EXPRESSION_APPLE:
    case BABY_TANK_EXPRESSION_BANANA:
    case BABY_TANK_EXPRESSION_CAKE:
    case BABY_TANK_EXPRESSION_COOKIE:
    case BABY_TANK_EXPRESSION_BREAD:
    case BABY_TANK_EXPRESSION_RICE_BALL:
    case BABY_TANK_EXPRESSION_ICE_CREAM:
    case BABY_TANK_EXPRESSION_MILK:
    case BABY_TANK_EXPRESSION_JUICE:
    case BABY_TANK_EXPRESSION_WATER:
    case BABY_TANK_EXPRESSION_YOGURT:
    case BABY_TANK_EXPRESSION_SOY_MILK:
    case BABY_TANK_EXPRESSION_COCOA:
        draw_outline_eye(76, 105, 26, 29, cyan);
        draw_outline_eye(164, 105, 26, 29, cyan);
        draw_bubble(120, 160, 8, amber);
        break;
    case BABY_TANK_EXPRESSION_COUNT:
        break;
    }
}

static bool on_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t *event_data,
                                   void *user_context)
{
    (void)panel_io;
    (void)event_data;
    (void)user_context;
    BaseType_t high_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_transfer_done, &high_priority_task_woken);
    return high_priority_task_woken == pdTRUE;
}

static esp_err_t wait_for_frame(void)
{
    if (!s_transfer_pending) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(FRAME_WAIT_MS)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "LCD frame transfer timed out");
    s_transfer_pending = false;
    return ESP_OK;
}

static esp_err_t configure_backlight(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << LCD_PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "backlight GPIO configuration failed");
    return gpio_set_level(LCD_PIN_BACKLIGHT, !LCD_BACKLIGHT_ON_LEVEL);
}

esp_err_t baby_tank_display_init(void)
{
    ESP_RETURN_ON_FALSE(s_panel == NULL, ESP_ERR_INVALID_STATE, TAG, "display already initialized");
    ESP_RETURN_ON_ERROR(configure_backlight(), TAG, "backlight setup failed");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus initialization failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.psram_dma_direct = 1,
    };
    esp_err_t ret = esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &s_panel_io);
    if (ret != ESP_OK) {
        spi_bus_free(LCD_HOST);
        ESP_LOGE(TAG, "panel IO creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        spi_bus_free(LCD_HOST);
        ESP_LOGE(TAG, "ST7789 driver creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_panel), fail, TAG, "panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_panel), fail, TAG, "panel initialization failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), fail, TAG, "panel inversion failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), fail, TAG, "panel enable failed");

    s_framebuffer = heap_caps_calloc(LCD_WIDTH * LCD_HEIGHT,
                                     sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ESP_GOTO_ON_FALSE(s_framebuffer != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "unable to allocate LCD framebuffer");
    s_transfer_done = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(s_transfer_done != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "unable to allocate LCD transfer semaphore");

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = on_color_transfer_done,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_panel_io, &callbacks, NULL),
                      fail, TAG, "LCD callback registration failed");

    ESP_LOGI(TAG,
             "ST7789 ready for animation; framebuffer=PSRAM-DMA inversion=on backlight=deferred");
    return ESP_OK;

fail:
    gpio_set_level(LCD_PIN_BACKLIGHT, !LCD_BACKLIGHT_ON_LEVEL);
    if (s_transfer_done != NULL) {
        vSemaphoreDelete(s_transfer_done);
        s_transfer_done = NULL;
    }
    if (s_framebuffer != NULL) {
        heap_caps_free(s_framebuffer);
        s_framebuffer = NULL;
    }
    if (s_panel != NULL) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_panel_io != NULL) {
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
    }
    spi_bus_free(LCD_HOST);
    return ret;
}

esp_err_t baby_tank_display_render_expression(baby_tank_expression_t expression,
                                              uint32_t elapsed_ms,
                                              uint32_t duration_ms,
                                              uint8_t intensity)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL && s_framebuffer != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "display is not initialized");
    ESP_RETURN_ON_FALSE(expression >= 0 && expression < BABY_TANK_EXPRESSION_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid expression");
    ESP_RETURN_ON_ERROR(wait_for_frame(), TAG, "previous frame is still pending");

    draw_expression(expression, elapsed_ms, duration_ms, intensity);
    draw_status_line();
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
        ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_BACKLIGHT, LCD_BACKLIGHT_ON_LEVEL),
                            TAG, "backlight enable failed");
        s_backlight_on = true;
    }
    return ESP_OK;
}

const char *baby_tank_expression_name(baby_tank_expression_t expression)
{
    static const char *const names[BABY_TANK_EXPRESSION_COUNT] = {
        "calm", "happy", "laughing", "excited", "love", "curious",
        "surprised", "shy", "proud", "bored", "annoyed", "complaining",
        "angry", "furious", "sad", "crying", "worried", "confused",
        "dizzy", "sick", "thinking", "sleepy", "sleeping", "bubble",
        "cannon", "listening", "processing", "eating", "drinking",
        "rock", "paper", "scissors", "fighting", "dice-1", "dice-2",
        "dice-3", "dice-4", "dice-5", "dice-6", "strawberry", "apple",
        "banana", "cake", "cookie", "bread", "rice-ball", "ice-cream",
        "milk", "juice", "water", "yogurt", "soy-milk", "cocoa",
    };
    if (expression < 0 || expression >= BABY_TANK_EXPRESSION_COUNT) {
        return "invalid";
    }
    return names[expression];
}
