#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BABY_TANK_AUDIO_SAMPLE_RATE 16000U

esp_err_t baby_tank_audio_io_init(void);
esp_err_t baby_tank_audio_io_read(int16_t *samples, size_t sample_count);
esp_err_t baby_tank_audio_io_begin_playback(void);
esp_err_t baby_tank_audio_io_write(int16_t *samples, size_t sample_count);
void baby_tank_audio_io_end_playback(void);
bool baby_tank_audio_io_is_playing(void);
esp_err_t baby_tank_audio_io_set_volume(uint8_t volume);
uint8_t baby_tank_audio_io_get_volume(void);
