#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Starts the board GC2145 and local-only HTTP camera service.
 * Port 80 exposes /health, /snapshot.jpg and the single-client /stream. */
esp_err_t baby_tank_camera_stream_start(void);
esp_err_t baby_tank_camera_stream_stop(void);
bool baby_tank_camera_stream_is_ready(void);
esp_err_t baby_tank_camera_stream_lan_info(uint8_t ip[4], uint8_t token[16]);
esp_err_t baby_tank_camera_stream_register_ap_routes(void);

/* Captures one JPEG under the same mutex as the local preview. Caller frees
 * the returned buffer. The image is never persisted on the device. */
esp_err_t baby_tank_camera_stream_capture_jpeg(uint8_t **jpeg, size_t *size);
