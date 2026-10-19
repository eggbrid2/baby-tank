#include "audio_io.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define AUDIO_I2C_PORT I2C_NUM_1
#define AUDIO_I2C_SDA GPIO_NUM_4
#define AUDIO_I2C_SCL GPIO_NUM_5
#define AUDIO_I2S_PORT I2S_NUM_1
#define AUDIO_I2S_MCLK GPIO_NUM_45
#define AUDIO_I2S_BCLK GPIO_NUM_39
#define AUDIO_I2S_WS GPIO_NUM_41
#define AUDIO_I2S_DOUT GPIO_NUM_42
#define AUDIO_I2S_DIN GPIO_NUM_40
#define AUDIO_IO_LOCK_MS 2000U
#define AUDIO_DEFAULT_VOLUME 75U
#define AUDIO_NVS_NAMESPACE "baby_audio"
#define AUDIO_NVS_VOLUME_KEY "volume"

static const char *TAG = "baby_tank_audio";
static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static esp_codec_dev_handle_t s_codec;
static SemaphoreHandle_t s_io_mutex;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_playing;
static uint8_t s_volume = AUDIO_DEFAULT_VOLUME;

static uint8_t load_saved_volume(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(AUDIO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) return AUDIO_DEFAULT_VOLUME;
    uint8_t volume = AUDIO_DEFAULT_VOLUME;
    result = nvs_get_u8(handle, AUDIO_NVS_VOLUME_KEY, &volume);
    nvs_close(handle);
    return result == ESP_OK && volume <= 100U
               ? volume : AUDIO_DEFAULT_VOLUME;
}

static esp_err_t save_volume(uint8_t volume)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(
        nvs_open(AUDIO_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG,
        "unable to open audio settings");
    esp_err_t result = nvs_set_u8(handle, AUDIO_NVS_VOLUME_KEY, volume);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

static esp_err_t initialize_i2c(void)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = AUDIO_I2C_PORT,
        .sda_io_num = AUDIO_I2C_SDA,
        .scl_io_num = AUDIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, &s_i2c_bus);
}

static esp_err_t initialize_i2s(void)
{
    i2s_chan_config_t channel =
        I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel.dma_desc_num = 8;
    channel.dma_frame_num = 320;
    channel.auto_clear = true;
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel, &s_tx_channel, &s_rx_channel), TAG,
        "unable to create duplex I2S channels");

    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BABY_TANK_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,
            .bclk = AUDIO_I2S_BCLK,
            .ws = AUDIO_I2S_WS,
            .dout = AUDIO_I2S_DOUT,
            .din = AUDIO_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    standard.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_channel, &standard),
                        TAG, "unable to configure I2S transmit");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, &standard),
                        TAG, "unable to configure I2S receive");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_channel), TAG,
                        "unable to enable I2S transmit");
    return i2s_channel_enable(s_rx_channel);
}

static esp_err_t initialize_codec(void)
{
    const audio_codec_data_if_t *data_interface;
    audio_codec_i2s_cfg_t i2s_config = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = s_rx_channel,
        .tx_handle = s_tx_channel,
    };
    data_interface = audio_codec_new_i2s_data(&i2s_config);
    ESP_RETURN_ON_FALSE(data_interface != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create codec I2S interface");

    audio_codec_i2c_cfg_t i2c_config = {
        .port = AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *control_interface =
        audio_codec_new_i2c_ctrl(&i2c_config);
    ESP_RETURN_ON_FALSE(control_interface != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create codec I2C interface");
    const audio_codec_gpio_if_t *gpio_interface = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_interface != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create codec GPIO interface");

    esp_codec_dev_hw_gain_t hardware_gain = {
        .pa_voltage = 5.0f,
        .codec_dac_voltage = 3.3f,
    };
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control_interface,
        .gpio_if = gpio_interface,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hardware_gain,
        .no_dac_ref = true,
    };
    const audio_codec_if_t *codec_interface = es8311_codec_new(&codec_config);
    ESP_RETURN_ON_FALSE(codec_interface != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create ES8311 interface");
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_interface,
        .data_if = data_interface,
    };
    s_codec = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create ES8311 device");

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = BABY_TANK_AUDIO_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_info) ==
                            ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "unable to open ES8311");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, 35.0f) ==
                            ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "unable to set microphone gain");
    s_volume = load_saved_volume();
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec, s_volume) ==
                            ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "unable to set speaker volume");
    ESP_LOGI(TAG, "speaker volume set to %u/100", s_volume);
    return ESP_OK;
}

esp_err_t baby_tank_audio_io_init(void)
{
    ESP_RETURN_ON_FALSE(s_codec == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "audio I/O already initialized");
    s_io_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_io_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "unable to create audio I/O mutex");
    ESP_RETURN_ON_ERROR(initialize_i2c(), TAG, "audio I2C init failed");
    ESP_RETURN_ON_ERROR(initialize_i2s(), TAG, "audio I2S init failed");
    ESP_RETURN_ON_ERROR(initialize_codec(), TAG, "ES8311 init failed");
    ESP_LOGI(TAG, "ES8311 duplex PCM ready: 16 kHz, 16-bit, mono");
    return ESP_OK;
}

esp_err_t baby_tank_audio_io_read(int16_t *samples, size_t sample_count)
{
    ESP_RETURN_ON_FALSE(samples != NULL && sample_count > 0U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid audio input buffer");
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "audio I/O is not initialized");
    ESP_RETURN_ON_FALSE(!baby_tank_audio_io_is_playing(),
                        ESP_ERR_INVALID_STATE, TAG, "speaker is active");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_io_mutex, pdMS_TO_TICKS(AUDIO_IO_LOCK_MS)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "audio input is busy");
    int result = esp_codec_dev_read(s_codec, samples,
                                    (int)(sample_count * sizeof(*samples)));
    xSemaphoreGive(s_io_mutex);
    return result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t baby_tank_audio_io_begin_playback(void)
{
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "audio I/O is not initialized");
    taskENTER_CRITICAL(&s_state_lock);
    s_playing = true;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t baby_tank_audio_io_write(int16_t *samples, size_t sample_count)
{
    ESP_RETURN_ON_FALSE(samples != NULL && sample_count > 0U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid playback buffer");
    ESP_RETURN_ON_FALSE(s_codec != NULL && baby_tank_audio_io_is_playing(),
                        ESP_ERR_INVALID_STATE, TAG, "playback is not active");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_io_mutex, pdMS_TO_TICKS(AUDIO_IO_LOCK_MS)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "audio output is busy");
    int result = esp_codec_dev_write(s_codec, samples,
                                     (int)(sample_count * sizeof(*samples)));
    xSemaphoreGive(s_io_mutex);
    return result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void baby_tank_audio_io_end_playback(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_playing = false;
    taskEXIT_CRITICAL(&s_state_lock);
}

bool baby_tank_audio_io_is_playing(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    bool playing = s_playing;
    taskEXIT_CRITICAL(&s_state_lock);
    return playing;
}

esp_err_t baby_tank_audio_io_set_volume(uint8_t volume)
{
    ESP_RETURN_ON_FALSE(volume <= 100U, ESP_ERR_INVALID_ARG, TAG,
                        "speaker volume must be 0-100");
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "audio I/O is not initialized");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_io_mutex, pdMS_TO_TICKS(AUDIO_IO_LOCK_MS)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "audio output is busy");
    int codec_result = esp_codec_dev_set_out_vol(s_codec, volume);
    xSemaphoreGive(s_io_mutex);
    ESP_RETURN_ON_FALSE(codec_result == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "unable to set speaker volume");

    taskENTER_CRITICAL(&s_state_lock);
    s_volume = volume;
    taskEXIT_CRITICAL(&s_state_lock);
    esp_err_t persist_result = save_volume(volume);
    if (persist_result != ESP_OK) {
        ESP_LOGW(TAG, "volume applied but not saved: %s",
                 esp_err_to_name(persist_result));
        return persist_result;
    }
    ESP_LOGI(TAG, "speaker volume set to %u/100", volume);
    return ESP_OK;
}

uint8_t baby_tank_audio_io_get_volume(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    uint8_t volume = s_volume;
    taskEXIT_CRITICAL(&s_state_lock);
    return volume;
}
