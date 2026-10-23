#include <assert.h>
#include <stdint.h>

#include "../main/policy_clock_math.h"

/* 2026-10-21 00:00:00 Asia/Shanghai. Test a full offline cycle and the
 * following morning; boundary instants belong to the new state. */
#define LOCAL_DAY_START_UTC_MS 1792512000000LL
#define HOUR_MS (60LL * 60LL * 1000LL)

int main(void)
{
    const int64_t day = LOCAL_DAY_START_UTC_MS;
    assert(baby_tank_shanghai_night_locked(day + 5 * HOUR_MS + 59 * 60000LL));
    assert(!baby_tank_shanghai_night_locked(day + 6 * HOUR_MS));
    assert(baby_tank_shanghai_presentation_daytime(day + 20 * HOUR_MS +
                                                  59 * 60000LL));
    assert(!baby_tank_shanghai_presentation_daytime(day + 21 * HOUR_MS));
    assert(!baby_tank_shanghai_night_locked(day + 22 * HOUR_MS + 59 * 60000LL));
    assert(baby_tank_shanghai_night_locked(day + 23 * HOUR_MS));
    assert(baby_tank_shanghai_night_locked(day + 29 * HOUR_MS + 59 * 60000LL));
    assert(!baby_tank_shanghai_night_locked(day + 30 * HOUR_MS));
    assert(!baby_tank_shanghai_night_locked(day + 30 * 24 * HOUR_MS +
                                           12 * HOUR_MS));
    return 0;
}
