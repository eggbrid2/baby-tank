#include <assert.h>
#include <stdint.h>

#include "control_protocol.h"

int main(void)
{
    const uint8_t drive[] = {1, 1, 42, 0, 0xe8, 0x03, 0x18, 0xfc};
    baby_tank_control_request_t request;
    assert(baby_tank_control_parse(drive, sizeof(drive), &request));
    assert(request.opcode == BABY_TANK_CONTROL_DRIVE);
    assert(request.sequence == 42);
    assert(request.left_per_mille == 1000);
    assert(request.right_per_mille == -1000);

    const uint8_t invalid_speed[] = {1, 1, 1, 0, 0xe9, 0x03, 0, 0};
    assert(!baby_tank_control_parse(invalid_speed, sizeof(invalid_speed),
                                    &request));

    const uint8_t wrong_version[] = {2, 0x7f, 1, 0, 0, 0, 0, 0};
    assert(!baby_tank_control_parse(wrong_version, sizeof(wrong_version),
                                    &request));

    const uint8_t ping[] = {1, 0x7f, 7, 0, 0, 0, 0, 0};
    assert(baby_tank_control_parse(ping, sizeof(ping), &request));
    const uint8_t invalid_stop[] = {1, 0, 8, 0, 1, 0, 0, 0};
    assert(!baby_tank_control_parse(invalid_stop, sizeof(invalid_stop), &request));
    const uint8_t invalid_flags[] = {1, 1, 8, 1, 0, 0, 0, 0};
    assert(!baby_tank_control_parse(invalid_flags, sizeof(invalid_flags), &request));
    assert(baby_tank_control_parse(ping, sizeof(ping), &request));
    uint8_t response[BABY_TANK_CONTROL_PACKET_SIZE];
    baby_tank_control_response(response, &request, BABY_TANK_CONTROL_OK);
    assert(response[0] == 1);
    assert(response[1] == 0xff);
    assert(response[2] == 7);
    assert(response[3] == 0);
    return 0;
}
