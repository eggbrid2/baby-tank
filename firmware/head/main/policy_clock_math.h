#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BABY_TANK_SHANGHAI_UTC_OFFSET_MS (8LL * 60LL * 60LL * 1000LL)
#define BABY_TANK_DAY_MS (24LL * 60LL * 60LL * 1000LL)
#define BABY_TANK_DAY_START_MS (6LL * 60LL * 60LL * 1000LL)
#define BABY_TANK_EVENING_START_MS (21LL * 60LL * 60LL * 1000LL)
#define BABY_TANK_NIGHT_START_MS (23LL * 60LL * 60LL * 1000LL)

static inline int64_t baby_tank_shanghai_local_ms(int64_t epoch_ms)
{
    return (epoch_ms + BABY_TANK_SHANGHAI_UTC_OFFSET_MS) % BABY_TANK_DAY_MS;
}

static inline bool baby_tank_shanghai_night_locked(int64_t epoch_ms)
{
    int64_t local_ms = baby_tank_shanghai_local_ms(epoch_ms);
    return local_ms < BABY_TANK_DAY_START_MS ||
           local_ms >= BABY_TANK_NIGHT_START_MS;
}

static inline bool baby_tank_shanghai_presentation_daytime(int64_t epoch_ms)
{
    int64_t local_ms = baby_tank_shanghai_local_ms(epoch_ms);
    return local_ms >= BABY_TANK_DAY_START_MS &&
           local_ms < BABY_TANK_EVENING_START_MS;
}
