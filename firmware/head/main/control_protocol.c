#include "control_protocol.h"

#include <string.h>

static int16_t read_i16_le(const uint8_t *value)
{
    return (int16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8U));
}

bool baby_tank_control_parse(const uint8_t *packet, size_t size,
                             baby_tank_control_request_t *request)
{
    if (packet == NULL || request == NULL ||
        size != BABY_TANK_CONTROL_PACKET_SIZE ||
        packet[0] != BABY_TANK_CONTROL_VERSION) {
        return false;
    }
    memset(request, 0, sizeof(*request));
    request->opcode = (baby_tank_control_opcode_t)packet[1];
    request->sequence = packet[2];
    request->flags = packet[3];
    request->left_per_mille = read_i16_le(packet + 4U);
    request->right_per_mille = read_i16_le(packet + 6U);
    if (request->left_per_mille < -1000 ||
        request->left_per_mille > 1000 ||
        request->right_per_mille < -1000 ||
        request->right_per_mille > 1000) {
        return false;
    }
    if (request->flags != 0U) return false;
    if ((request->opcode == BABY_TANK_CONTROL_STOP ||
         request->opcode == BABY_TANK_CONTROL_PING) &&
        (request->left_per_mille != 0 || request->right_per_mille != 0)) {
        return false;
    }
    return request->opcode == BABY_TANK_CONTROL_STOP ||
           request->opcode == BABY_TANK_CONTROL_DRIVE ||
           request->opcode == BABY_TANK_CONTROL_LIGHTS ||
           request->opcode == BABY_TANK_CONTROL_PING;
}

void baby_tank_control_response(uint8_t *packet,
                                const baby_tank_control_request_t *request,
                                baby_tank_control_status_t status)
{
    memset(packet, 0, BABY_TANK_CONTROL_PACKET_SIZE);
    packet[0] = BABY_TANK_CONTROL_VERSION;
    packet[1] = request == NULL ? 0xffU : (uint8_t)request->opcode | 0x80U;
    packet[2] = request == NULL ? 0U : request->sequence;
    packet[3] = (uint8_t)status;
}
