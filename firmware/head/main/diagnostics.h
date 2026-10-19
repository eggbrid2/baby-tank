#pragma once

#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

#define BABY_TANK_DIAGNOSTICS_VERSION 1U
#define BABY_TANK_DIAGNOSTICS_REQUEST_SIZE 8U
#define BABY_TANK_DIAGNOSTICS_RESPONSE_SIZE 40U

/* Handler for the authenticated provisioning transport custom endpoint. */
esp_err_t baby_tank_diagnostics_endpoint(uint32_t session_id,
                                         const uint8_t *input,
                                         ssize_t input_size,
                                         uint8_t **output,
                                         ssize_t *output_size,
                                         void *context);
