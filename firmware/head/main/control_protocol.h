#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BABY_TANK_CONTROL_PACKET_SIZE 8U
#define BABY_TANK_CONTROL_VERSION 1U

typedef enum {
    BABY_TANK_CONTROL_STOP = 0x00,
    BABY_TANK_CONTROL_DRIVE = 0x01,
    BABY_TANK_CONTROL_LIGHTS = 0x02,
    BABY_TANK_CONTROL_PING = 0x7f,
} baby_tank_control_opcode_t;

typedef enum {
    BABY_TANK_CONTROL_OK = 0,
    BABY_TANK_CONTROL_INVALID = 1,
    BABY_TANK_CONTROL_UNAVAILABLE = 2,
} baby_tank_control_status_t;

typedef struct {
    baby_tank_control_opcode_t opcode;
    uint8_t sequence;
    uint8_t flags;
    int16_t left_per_mille;
    int16_t right_per_mille;
} baby_tank_control_request_t;

bool baby_tank_control_parse(const uint8_t *packet, size_t size,
                             baby_tank_control_request_t *request);
void baby_tank_control_response(uint8_t *packet,
                                const baby_tank_control_request_t *request,
                                baby_tank_control_status_t status);
